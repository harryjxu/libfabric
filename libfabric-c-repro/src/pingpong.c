/*
 * pingpong.c - Ping-pong test matching fabulous implementation exactly
 * 
 * This test creates 3 threads total:
 * - Main thread: orchestrates the test
 * - Sender's completion thread: handles all operations for device 11
 * - Receiver's completion thread: handles all operations for device 3
 */

#include "common.h"
#include "completion_thread.h"
#include <semaphore.h>

/* Test parameters matching fabulous_ping_pong_many.py */
#define FROM_DEVICE 11
#define TO_DEVICE 3
#define TAG 1234
#undef BUFFER_SIZE  // Undefine the one from common.h
#define BUFFER_SIZE (400 * 1024 * 1024)  // 400MB like fabulous test

/* Domain context */
struct domain_context {
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct completion_thread *cq_thread;
    int device_id;
    char device_name[64];
};

/* Endpoint context */
struct endpoint_context {
    struct fid_ep *ep;
    struct fid_av *av;
    void *address;
    size_t address_len;
    struct fid_mr *mr;
    void *buffer;
    size_t buffer_size;
};

/* Transfer completion tracking */
struct transfer_completion {
    sem_t sem;
    int status;
    volatile int done;
};

static void transfer_complete(void *context, int status) {
    struct transfer_completion *comp = context;
    comp->status = status;
    comp->done = 1;
    sem_post(&comp->sem);
}

/* Initialize domain matching fabulous efa_domains() */
static int init_domain(struct domain_context *ctx, int device_id) {
    struct fi_info *hints;
    int ret;
    
    ctx->device_id = device_id;
    snprintf(ctx->device_name, sizeof(ctx->device_name), "device_%d", device_id);
    
    /* Setup hints matching fabulous */
    hints = fi_allocinfo();
    if (!hints) {
        LOG_ERROR("Failed to allocate hints");
        return -1;
    }
    
    hints->fabric_attr->prov_name = strdup(EFA_PROVIDER);
    hints->ep_attr->type = FI_EP_RDM;
    hints->caps = FI_ATOMIC | FI_MSG | FI_READ | FI_RECV | 
                  FI_REMOTE_READ | FI_REMOTE_WRITE | FI_RMA | 
                  FI_SEND | FI_TAGGED | FI_WRITE;
    hints->domain_attr->mr_mode = FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | 
                                  FI_MR_PROV_KEY | FI_MR_LOCAL;
    hints->domain_attr->threading = FI_THREAD_SAFE;
    
    /* Get fabric info */
    ret = fi_getinfo(FABRIC_VERSION, NULL, NULL, 0, hints, &ctx->info);
    if (ret) {
        LOG_ERROR("fi_getinfo failed: %s", fi_strerror(-ret));
        fi_freeinfo(hints);
        return ret;
    }
    
    /* TODO: Here we should iterate through the returned info list and
     * find the one matching our desired device_id. For now, we'll just
     * use the first one, but in the real implementation we'd match
     * based on PCI address or domain name.
     */
    
    /* Create fabric */
    ret = fi_fabric(ctx->info->fabric_attr, &ctx->fabric, NULL);
    if (ret) {
        LOG_ERROR("fi_fabric failed: %s", fi_strerror(-ret));
        fi_freeinfo(hints);
        return ret;
    }
    
    /* Create domain */
    ret = fi_domain(ctx->fabric, ctx->info, &ctx->domain, NULL);
    if (ret) {
        LOG_ERROR("fi_domain failed: %s", fi_strerror(-ret));
        fi_close(&ctx->fabric->fid);
        fi_freeinfo(hints);
        return ret;
    }
    
    /* Create completion thread for this domain - matching cq_thread_per_domain=true */
    ctx->cq_thread = completion_thread_create(ctx->domain);
    if (!ctx->cq_thread) {
        LOG_ERROR("Failed to create completion thread");
        fi_close(&ctx->domain->fid);
        fi_close(&ctx->fabric->fid);
        fi_freeinfo(hints);
        return -1;
    }
    
    fi_freeinfo(hints);
    LOG_INFO("Initialized domain for device %d", device_id);
    return 0;
}

/* Create endpoint via completion thread */
static int create_endpoint(struct domain_context *domain_ctx, 
                          struct endpoint_context *ep_ctx) {
    int resp_pipe[2];
    struct operation_request req = {0};
    struct endpoint_response resp = {0};
    
    /* Create response pipe */
    if (pipe(resp_pipe) < 0) {
        LOG_ERROR("Failed to create response pipe");
        return -1;
    }
    
    /* Send create endpoint request */
    req.type = OP_CREATE_ENDPOINT;
    req.create_ep.response_fd = resp_pipe[1];
    
    if (completion_thread_send_op(domain_ctx->cq_thread, &req) < 0) {
        close(resp_pipe[0]);
        close(resp_pipe[1]);
        return -1;
    }
    
    /* Wait for response - don't close write end yet! */
    ssize_t n = read(resp_pipe[0], &resp, sizeof(resp));
    close(resp_pipe[0]);
    close(resp_pipe[1]);  // Now close write end
    
    if (n != sizeof(resp)) {
        LOG_ERROR("Failed to read endpoint response");
        return -1;
    }
    
    if (!resp.ep || !resp.av) {
        LOG_ERROR("Endpoint creation failed");
        if (resp.address) free(resp.address);
        return -1;
    }
    
    /* Save endpoint info */
    ep_ctx->ep = resp.ep;
    ep_ctx->av = resp.av;
    ep_ctx->address = resp.address;
    ep_ctx->address_len = resp.address_len;
    
    LOG_INFO("Created endpoint %p with address length %zu", 
             ep_ctx->ep, ep_ctx->address_len);
    return 0;
}

/* Allocate and register memory */
static int allocate_buffer(struct domain_context *domain_ctx,
                          struct endpoint_context *ep_ctx,
                          size_t size) {
    int ret;
    
    /* Allocate buffer */
    ep_ctx->buffer = malloc(size);
    if (!ep_ctx->buffer) {
        LOG_ERROR("Failed to allocate buffer of size %zu", size);
        return -ENOMEM;
    }
    ep_ctx->buffer_size = size;
    
    /* Register memory */
    ret = fi_mr_reg(domain_ctx->domain, ep_ctx->buffer, size,
                    FI_SEND | FI_RECV, 0, 0, 0, &ep_ctx->mr, NULL);
    if (ret) {
        LOG_ERROR("fi_mr_reg failed: %s", fi_strerror(-ret));
        free(ep_ctx->buffer);
        ep_ctx->buffer = NULL;
        return ret;
    }
    
    return 0;
}

/* Send data matching fabulous start_tsend */
static int do_send(struct domain_context *domain_ctx,
                  struct endpoint_context *ep_ctx,
                  void *peer_addr, size_t peer_addr_len,
                  uint64_t tag, struct transfer_completion *comp) {
    struct operation_request req = {0};
    
    req.type = OP_TSEND;
    req.transfer.ep = ep_ctx->ep;
    req.transfer.buf = ep_ctx->buffer;
    req.transfer.len = ep_ctx->buffer_size;
    req.transfer.desc = fi_mr_desc(ep_ctx->mr);
    req.transfer.peer_addr = peer_addr;
    req.transfer.peer_addr_len = peer_addr_len;
    req.transfer.tag = tag;
    req.transfer.context = comp;
    req.transfer.completion_fn = transfer_complete;
    
    return completion_thread_send_op(domain_ctx->cq_thread, &req);
}

/* Receive data matching fabulous start_trecv */
static int do_recv(struct domain_context *domain_ctx,
                  struct endpoint_context *ep_ctx,
                  void *peer_addr, size_t peer_addr_len,
                  uint64_t tag, struct transfer_completion *comp) {
    struct operation_request req = {0};
    
    req.type = OP_TRECV;
    req.transfer.ep = ep_ctx->ep;
    req.transfer.buf = ep_ctx->buffer;
    req.transfer.len = ep_ctx->buffer_size;
    req.transfer.desc = fi_mr_desc(ep_ctx->mr);
    req.transfer.peer_addr = peer_addr;
    req.transfer.peer_addr_len = peer_addr_len;
    req.transfer.tag = tag;
    req.transfer.ignore = 0;
    req.transfer.context = comp;
    req.transfer.completion_fn = transfer_complete;
    
    return completion_thread_send_op(domain_ctx->cq_thread, &req);
}

/* Wait for transfer completion */
static int wait_for_completion(struct transfer_completion *comp, const char *op_name) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 30;  // 30 second timeout
    
    if (sem_timedwait(&comp->sem, &ts) < 0) {
        LOG_ERROR("%s timed out", op_name);
        return -ETIMEDOUT;
    }
    
    if (comp->status != 0) {
        LOG_ERROR("%s failed with status %d", op_name, comp->status);
        return comp->status;
    }
    
    LOG_DEBUG("%s completed successfully", op_name);
    return 0;
}

/* Run ping-pong transfer */
static int run_pingpong(struct domain_context *from_domain,
                       struct endpoint_context *from_ep,
                       struct domain_context *to_domain,
                       struct endpoint_context *to_ep,
                       uint64_t tag,
                       const char *phase) {
    struct transfer_completion send_comp = {0};
    struct transfer_completion recv_comp = {0};
    double start_time, end_time;
    int ret;
    
    sem_init(&send_comp.sem, 0, 0);
    sem_init(&recv_comp.sem, 0, 0);
    
    LOG_INFO("Starting %s transfer...", phase);
    start_time = get_time_ms();
    
    /* Start receive first (matching fabulous pattern) */
    ret = do_recv(to_domain, to_ep, from_ep->address, from_ep->address_len,
                  tag, &recv_comp);
    if (ret) {
        LOG_ERROR("Failed to start receive");
        goto cleanup;
    }
    
    /* Then start send */
    ret = do_send(from_domain, from_ep, to_ep->address, to_ep->address_len,
                  tag, &send_comp);
    if (ret) {
        LOG_ERROR("Failed to start send");
        goto cleanup;
    }
    
    /* Wait for both to complete */
    ret = wait_for_completion(&recv_comp, "receive");
    if (ret) goto cleanup;
    
    ret = wait_for_completion(&send_comp, "send");
    if (ret) goto cleanup;
    
    end_time = get_time_ms();
    
    /* Calculate throughput */
    double duration_ms = end_time - start_time;
    double throughput_gbps = (from_ep->buffer_size * 8.0) / (duration_ms * 1e6);
    
    LOG_INFO("%s completed in %.2f ms (%.2f Gbps)", 
             phase, duration_ms, throughput_gbps);
    
cleanup:
    sem_destroy(&send_comp.sem);
    sem_destroy(&recv_comp.sem);
    return ret;
}

int main(void) {
    struct domain_context from_domain = {0};
    struct domain_context to_domain = {0};
    struct endpoint_context from_ep = {0};
    struct endpoint_context to_ep = {0};
    int ret = 0;
    
    LOG_INFO("=== Ping-Pong Test (Fabulous-style) ===");
    LOG_INFO("Buffer size: %d MB", BUFFER_SIZE / (1024 * 1024));
    LOG_INFO("From device: %d, To device: %d", FROM_DEVICE, TO_DEVICE);
    LOG_INFO("Tag: %d", TAG);
    
    /* Initialize domains with completion threads */
    LOG_INFO("\nInitializing domains...");
    ret = init_domain(&from_domain, FROM_DEVICE);
    if (ret) {
        LOG_ERROR("Failed to initialize from domain");
        goto cleanup;
    }
    
    ret = init_domain(&to_domain, TO_DEVICE);
    if (ret) {
        LOG_ERROR("Failed to initialize to domain");
        goto cleanup;
    }
    
    /* Create endpoints via completion threads */
    LOG_INFO("\nCreating endpoints...");
    ret = create_endpoint(&from_domain, &from_ep);
    if (ret) {
        LOG_ERROR("Failed to create from endpoint");
        goto cleanup;
    }
    
    ret = create_endpoint(&to_domain, &to_ep);
    if (ret) {
        LOG_ERROR("Failed to create to endpoint");
        goto cleanup;
    }
    
    /* Allocate and register buffers */
    LOG_INFO("\nAllocating buffers...");
    ret = allocate_buffer(&from_domain, &from_ep, BUFFER_SIZE);
    if (ret) {
        LOG_ERROR("Failed to allocate from buffer");
        goto cleanup;
    }
    
    /* Fill send buffer with test pattern */
    memset(from_ep.buffer, 0xAB, BUFFER_SIZE);
    
    ret = allocate_buffer(&to_domain, &to_ep, BUFFER_SIZE);
    if (ret) {
        LOG_ERROR("Failed to allocate to buffer");
        goto cleanup;
    }
    
    /* Clear receive buffer */
    memset(to_ep.buffer, 0, BUFFER_SIZE);
    
    /* Run warmup handshake (matching fabulous) */
    LOG_INFO("\nRunning warmup handshake...");
    ret = run_pingpong(&from_domain, &from_ep, &to_domain, &to_ep, TAG, "warmup");
    if (ret) {
        LOG_ERROR("Warmup failed");
        goto cleanup;
    }
    
    /* Verify data was transferred */
    if (memcmp(from_ep.buffer, to_ep.buffer, BUFFER_SIZE) != 0) {
        LOG_ERROR("Data verification failed after warmup!");
        ret = -1;
        goto cleanup;
    }
    LOG_INFO("✓ Data verified after warmup");
    
    /* Clear receive buffer again */
    memset(to_ep.buffer, 0, BUFFER_SIZE);
    
    /* Run actual transfer (matching fabulous) */
    LOG_INFO("\nRunning actual transfer...");
    ret = run_pingpong(&from_domain, &from_ep, &to_domain, &to_ep, TAG + 1, "actual");
    if (ret) {
        LOG_ERROR("Actual transfer failed");
        goto cleanup;
    }
    
    /* Final data verification */
    if (memcmp(from_ep.buffer, to_ep.buffer, BUFFER_SIZE) != 0) {
        LOG_ERROR("Data verification failed after actual transfer!");
        ret = -1;
        goto cleanup;
    }
    LOG_INFO("✓ Data verified after actual transfer");
    
    LOG_INFO("\n✅ SUCCESS: Ping-pong test completed successfully!");
    
cleanup:
    /* Cleanup in reverse order */
    if (to_ep.mr) fi_close(&to_ep.mr->fid);
    if (to_ep.buffer) free(to_ep.buffer);
    if (to_ep.address) free(to_ep.address);
    
    if (from_ep.mr) fi_close(&from_ep.mr->fid);
    if (from_ep.buffer) free(from_ep.buffer);
    if (from_ep.address) free(from_ep.address);
    
    /* Endpoints are closed by completion threads */
    
    if (to_domain.cq_thread) completion_thread_destroy(to_domain.cq_thread);
    if (to_domain.domain) fi_close(&to_domain.domain->fid);
    if (to_domain.fabric) fi_close(&to_domain.fabric->fid);
    if (to_domain.info) fi_freeinfo(to_domain.info);
    
    if (from_domain.cq_thread) completion_thread_destroy(from_domain.cq_thread);
    if (from_domain.domain) fi_close(&from_domain.domain->fid);
    if (from_domain.fabric) fi_close(&from_domain.fabric->fid);
    if (from_domain.info) fi_freeinfo(from_domain.info);
    
    return ret;
}