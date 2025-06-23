/*
 * repro_bug.c - Reproduces the libfabric endpoint close bug
 * 
 * This test creates multiple endpoints and randomly kills them during
 * active transfers to trigger the assertion error in libfabric.
 */

#include "common.h"
#include "completion_thread.h"
#include <semaphore.h>
#include <signal.h>

/* Test parameters */
#define NUM_ENDPOINTS 4
#define TRANSFER_SIZE 512  // 512 bytes like Python test
#define MAX_ITERATIONS 100000
#define KILL_PROBABILITY 0.01  // 1% chance to kill endpoint per iteration
#define TAG_BASE 1000  // Match Python test tag base
#define MIN_KILL_DELAY_MS 0   // Can kill immediately
#define MAX_KILL_DELAY_MS 20  // Match Python test MAX_KILL_DELAY_MS


/* Domain context */
struct domain_context {
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct completion_thread *cq_thread;
    char name[64];
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
    int alive;  // 1 if endpoint is alive, 0 if killed
    int id;
};

/* Transfer completion tracking */
struct transfer_completion {
    sem_t sem;
    int status;
    volatile int done;
};

static void transfer_complete(void *context, int status) {
    struct transfer_completion *comp = context;
    if (!comp) {
        LOG_ERROR("NULL completion context!");
        assert(0 && "NULL completion context in transfer_complete");
        return;
    }
    comp->status = status;
    comp->done = 1;
    int ret = sem_post(&comp->sem);
    if (ret != 0) {
        LOG_ERROR("sem_post failed: %s", strerror(errno));
        assert(0 && "sem_post failed");
    }
}

/* Initialize domain */
static int init_domain(struct domain_context *ctx, const char *name) {
    struct fi_info *hints;
    int ret;
    
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);
    
    hints = fi_allocinfo();
    if (!hints) {
        LOG_ERROR("Failed to allocate hints");
        return -1;
    }
    
    hints->fabric_attr->prov_name = strdup(EFA_PROVIDER);
    hints->ep_attr->type = FI_EP_RDM;
    hints->caps = FI_MSG | FI_TAGGED | FI_SEND | FI_RECV;
    hints->domain_attr->mr_mode = FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | 
                                  FI_MR_PROV_KEY | FI_MR_LOCAL;
    hints->domain_attr->threading = FI_THREAD_SAFE;
    
    ret = fi_getinfo(FABRIC_VERSION, NULL, NULL, 0, hints, &ctx->info);
    if (ret) {
        LOG_ERROR("fi_getinfo failed: %s", fi_strerror(-ret));
        fi_freeinfo(hints);
        return ret;
    }
    
    ret = fi_fabric(ctx->info->fabric_attr, &ctx->fabric, NULL);
    if (ret) {
        LOG_ERROR("fi_fabric failed: %s", fi_strerror(-ret));
        fi_freeinfo(hints);
        return ret;
    }
    
    ret = fi_domain(ctx->fabric, ctx->info, &ctx->domain, NULL);
    if (ret) {
        LOG_ERROR("fi_domain failed: %s", fi_strerror(-ret));
        fi_close(&ctx->fabric->fid);
        fi_freeinfo(hints);
        return ret;
    }
    
    ctx->cq_thread = completion_thread_create(ctx->domain);
    if (!ctx->cq_thread) {
        LOG_ERROR("Failed to create completion thread");
        fi_close(&ctx->domain->fid);
        fi_close(&ctx->fabric->fid);
        fi_freeinfo(hints);
        return -1;
    }
    
    fi_freeinfo(hints);
    LOG_INFO("Initialized domain %s", name);
    return 0;
}

/* Create endpoint via completion thread */
static int create_endpoint(struct domain_context *domain_ctx, 
                          struct endpoint_context *ep_ctx,
                          int id) {
    int resp_pipe[2];
    struct operation_request req = {0};
    struct endpoint_response resp = {0};
    
    ep_ctx->id = id;
    ep_ctx->alive = 1;
    
    if (pipe(resp_pipe) < 0) {
        LOG_ERROR("Failed to create response pipe");
        return -1;
    }
    
    req.type = OP_CREATE_ENDPOINT;
    req.create_ep.response_fd = resp_pipe[1];
    
    if (completion_thread_send_op(domain_ctx->cq_thread, &req) < 0) {
        close(resp_pipe[0]);
        close(resp_pipe[1]);
        return -1;
    }
    
    ssize_t n = read(resp_pipe[0], &resp, sizeof(resp));
    close(resp_pipe[0]);
    close(resp_pipe[1]);
    
    if (n != sizeof(resp)) {
        LOG_ERROR("Failed to read endpoint response");
        return -1;
    }
    
    if (!resp.ep || !resp.av) {
        LOG_ERROR("Endpoint creation failed");
        if (resp.address) free(resp.address);
        return -1;
    }
    
    ep_ctx->ep = resp.ep;
    ep_ctx->av = resp.av;
    ep_ctx->address = resp.address;
    ep_ctx->address_len = resp.address_len;
    
    /* Allocate and register buffer */
    ep_ctx->buffer = malloc(TRANSFER_SIZE);
    if (!ep_ctx->buffer) {
        LOG_ERROR("Failed to allocate buffer");
        free(ep_ctx->address);
        return -ENOMEM;
    }
    ep_ctx->buffer_size = TRANSFER_SIZE;
    
    int ret = fi_mr_reg(domain_ctx->domain, ep_ctx->buffer, TRANSFER_SIZE,
                        FI_SEND | FI_RECV | FI_READ | FI_WRITE, 0, 0, 0, &ep_ctx->mr, NULL);
    if (ret) {
        LOG_ERROR("fi_mr_reg failed: %s", fi_strerror(-ret));
        free(ep_ctx->buffer);
        free(ep_ctx->address);
        return ret;
    }
    
    LOG_INFO("Created endpoint %d", id);
    return 0;
}

/* Kill endpoint - this should trigger the bug */
static void kill_endpoint(struct domain_context *domain_ctx, struct endpoint_context *ep_ctx) {
    if (!ep_ctx->alive) {
        return;
    }
    
    LOG_INFO("🔪 KILLING endpoint %d", ep_ctx->id);
    ep_ctx->alive = 0;
    
    /* Simulate Python's GC/refcount delay before close */
    sleep_ms(15);  // 15ms to simulate Python garbage collection pause
    
    /* Send close operation to completion thread - EXACTLY like fabulous!
     * We must close in the completion thread to avoid race conditions */
    if (ep_ctx->ep) {
        struct operation_request req = {0};
        req.type = OP_CLOSE_ENDPOINT;
        req.transfer.ep = ep_ctx->ep;
        
        if (completion_thread_send_op(domain_ctx->cq_thread, &req) < 0) {
            LOG_ERROR("Failed to send close operation to completion thread");
        }
        
        /* Clear our reference - completion thread will handle the actual close */
        ep_ctx->ep = NULL;
        ep_ctx->av = NULL;
    }
}

/* Start a transfer */
static int start_transfer(struct domain_context *sender_domain,
                         struct domain_context *receiver_domain,
                         struct endpoint_context *from_ep,
                         struct endpoint_context *to_ep,
                         uint64_t tag,
                         struct transfer_completion *send_comp,
                         struct transfer_completion *recv_comp) {
    assert(sender_domain != NULL && "start_transfer: sender_domain is NULL");
    assert(receiver_domain != NULL && "start_transfer: receiver_domain is NULL");
    assert(from_ep != NULL && "start_transfer: from_ep is NULL");
    assert(to_ep != NULL && "start_transfer: to_ep is NULL");
    assert(send_comp != NULL && "start_transfer: send_comp is NULL");
    assert(recv_comp != NULL && "start_transfer: recv_comp is NULL");
    assert(from_ep->ep != NULL && "start_transfer: from_ep->ep is NULL");
    assert(to_ep->ep != NULL && "start_transfer: to_ep->ep is NULL");
    assert(from_ep->buffer != NULL && "start_transfer: from_ep->buffer is NULL");
    assert(to_ep->buffer != NULL && "start_transfer: to_ep->buffer is NULL");
    assert(from_ep->mr != NULL && "start_transfer: from_ep->mr is NULL");
    assert(to_ep->mr != NULL && "start_transfer: to_ep->mr is NULL");
    
    struct operation_request req = {0};
    
    /* Start receive */
    req.type = OP_TRECV;
    req.transfer.ep = to_ep->ep;
    req.transfer.buf = to_ep->buffer;
    req.transfer.len = to_ep->buffer_size;
    req.transfer.desc = fi_mr_desc(to_ep->mr);
    req.transfer.peer_addr = from_ep->address;
    req.transfer.peer_addr_len = from_ep->address_len;
    req.transfer.tag = tag;
    req.transfer.ignore = 0;
    req.transfer.context = recv_comp;
    req.transfer.completion_fn = transfer_complete;
    
    if (completion_thread_send_op(receiver_domain->cq_thread, &req) < 0) {
        return -1;
    }
    
    /* Simulate Python's async task scheduling delay */
    sleep_ms(2);  // 2ms between recv and send operations
    
    /* Start send */
    req.type = OP_TSEND;
    req.transfer.ep = from_ep->ep;
    req.transfer.buf = from_ep->buffer;
    req.transfer.len = from_ep->buffer_size;
    req.transfer.desc = fi_mr_desc(from_ep->mr);
    req.transfer.peer_addr = to_ep->address;
    req.transfer.peer_addr_len = to_ep->address_len;
    req.transfer.tag = tag;
    req.transfer.context = send_comp;
    req.transfer.completion_fn = transfer_complete;
    
    return completion_thread_send_op(sender_domain->cq_thread, &req);
}

int main(void) {
    struct domain_context sender_domain = {0};
    struct domain_context receiver_domain = {0};
    struct endpoint_context sender_ep = {0};
    struct endpoint_context receiver_ep = {0};
    int ret = 0;
    int iteration = 0;
    int errors = 0;
    
    LOG_INFO("=== Endpoint Close Bug Reproduction Test ===");
    LOG_INFO("This test randomly kills endpoints during transfers to trigger assertions");
    LOG_INFO("Kill delay range: %d-%dms", MIN_KILL_DELAY_MS, MAX_KILL_DELAY_MS);
    
    /* Set environment variables like Python test */
    setenv("FABULOUS_EP_BIND_DELAY_MS", "30", 1);  // Increased to simulate Python delays
    setenv("FI_LOG_LEVEL", "warn", 1);  // Reduce noise during tests
    
    /* Seed random number generator - hardcoded for reproducibility */
    unsigned int seed = 42;
    LOG_INFO("Using hardcoded seed: %u", seed);
    srand(seed);
    
    /* Initialize domains - matching Python's device 11 and 3 */
    ret = init_domain(&sender_domain, "sender_domain");
    if (ret) {
        LOG_ERROR("Failed to initialize sender domain");
        return ret;
    }
    
    ret = init_domain(&receiver_domain, "receiver_domain");
    if (ret) {
        LOG_ERROR("Failed to initialize receiver domain");
        goto cleanup;
    }
    
    LOG_INFO("Starting test iterations...");
    double start_time = get_time_ms();
    
    /* Main test loop */
    for (iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
        uint64_t tag = TAG_BASE + iteration;
        
        /* Create fresh endpoints for each iteration - matching Python */
        ret = create_endpoint(&sender_domain, &sender_ep, 0);
        if (ret) {
            LOG_ERROR("Failed to create sender endpoint");
            errors++;
            if (errors > 10) break;
            continue;
        }
        
        ret = create_endpoint(&receiver_domain, &receiver_ep, 1);
        if (ret) {
            LOG_ERROR("Failed to create receiver endpoint");
            /* Clean up sender */
            if (sender_ep.mr) fi_close(&sender_ep.mr->fid);
            if (sender_ep.buffer) free(sender_ep.buffer);
            if (sender_ep.address) free(sender_ep.address);
            errors++;
            if (errors > 10) break;
            continue;
        }
        
        /* Simulate Python's object creation overhead */
        sleep_ms(8);  // 8ms to simulate Python GC/object allocation delays
        
        /* Decide kill scenario - matching Python */
        int scenario = rand() % 4;
        const char *scenario_name;
        switch (scenario) {
            case 0: scenario_name = "none"; break;
            case 1: scenario_name = "kill_sender"; break;
            case 2: scenario_name = "kill_receiver"; break;
            case 3: scenario_name = "kill_both"; break;
        }
        
        /* Random kill delay MIN-MAX ms */
        int kill_delay_ms = MIN_KILL_DELAY_MS + (rand() % (MAX_KILL_DELAY_MS - MIN_KILL_DELAY_MS + 1));
        
        /* Log every 100 iterations or when killing */
        if (iteration % 100 == 0 || scenario != 0) {
            double elapsed = get_time_ms() - start_time;
            LOG_INFO("[Iter %d @ %.1fs] Scenario: %s, delay: %dms", 
                     iteration, elapsed / 1000.0, scenario_name, kill_delay_ms);
        }
        
        /* Start transfer - allocate on heap to avoid use-after-free */
        struct transfer_completion *send_comp = NULL;
        struct transfer_completion *recv_comp = NULL;
        send_comp = calloc(1, sizeof(struct transfer_completion));
        recv_comp = calloc(1, sizeof(struct transfer_completion));
        if (!send_comp || !recv_comp) {
            LOG_ERROR("Failed to allocate completion structures");
            assert(0 && "Failed to allocate completion structures");
            free(send_comp);
            free(recv_comp);
            goto iter_cleanup;
        }
        assert(send_comp != NULL && "send_comp allocation check");
        assert(recv_comp != NULL && "recv_comp allocation check");
        
        int ret1 = sem_init(&send_comp->sem, 0, 0);
        int ret2 = sem_init(&recv_comp->sem, 0, 0);
        if (ret1 != 0 || ret2 != 0) {
            LOG_ERROR("sem_init failed: %s", strerror(errno));
            assert(0 && "sem_init failed");
        }
        
        ret = start_transfer(&sender_domain, &receiver_domain, 
                           &sender_ep, &receiver_ep,
                           tag, send_comp, recv_comp);
        if (ret) {
            LOG_ERROR("Failed to start transfer");
            goto iter_cleanup;
        }
        
        /* Simulate Python's trio task startup delay */
        sleep_ms(1);  // 1ms for async task scheduling overhead
        
        /* Sleep before killing (allow 0 delay for immediate kill) */
        if (scenario != 0 && kill_delay_ms > 0) {
            sleep_ms(kill_delay_ms);
        }
        
        /* Execute kill scenario */
        switch (scenario) {
            case 1:  /* kill_sender */
                double elapsed1 = get_time_ms() - start_time;
                LOG_INFO("[Iter %d @ %.1fs] Killing SENDER endpoint after %dms", 
                         iteration, elapsed1 / 1000.0, kill_delay_ms);
                kill_endpoint(&sender_domain, &sender_ep);
                /* Hard kill - leak completion structures to avoid use-after-free 
                 * In a real app, we'd track and clean these up properly */
                continue;
            case 2:  /* kill_receiver */
                double elapsed2 = get_time_ms() - start_time;
                LOG_INFO("[Iter %d @ %.1fs] Killing RECEIVER endpoint after %dms", 
                         iteration, elapsed2 / 1000.0, kill_delay_ms);
                kill_endpoint(&receiver_domain, &receiver_ep);
                /* Hard kill - leak completion structures to avoid use-after-free 
                 * In a real app, we'd track and clean these up properly */
                continue;
            case 3:  /* kill_both */
                double elapsed3 = get_time_ms() - start_time;
                LOG_INFO("[Iter %d @ %.1fs] Killing BOTH endpoints after %dms", 
                         iteration, elapsed3 / 1000.0, kill_delay_ms);
                kill_endpoint(&sender_domain, &sender_ep);
                kill_endpoint(&receiver_domain, &receiver_ep);
                /* Hard kill - leak completion structures to avoid use-after-free 
                 * In a real app, we'd track and clean these up properly */
                continue;
        }
        
        /* Only wait for completion if we didn't kill anything */
        if (scenario == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  // 1 second timeout for normal completion
            
            sem_timedwait(&send_comp->sem, &ts);
            sem_timedwait(&recv_comp->sem, &ts);
        }
        
iter_cleanup:
        /* Only free if we didn't start a transfer or if we waited for completion */
        if (send_comp && recv_comp && scenario == 0) {
            /* Normal completion path - safe to free */
            sem_destroy(&send_comp->sem);
            free(send_comp);
            sem_destroy(&recv_comp->sem);  
            free(recv_comp);
        }
        /* If we killed endpoints, we leaked the completion structures on purpose */
        
        /* Always close endpoints (if not already closed by kill_endpoint) */
        if (sender_ep.alive && sender_ep.ep) {
            kill_endpoint(&sender_domain, &sender_ep);
        }
        if (receiver_ep.alive && receiver_ep.ep) {
            kill_endpoint(&receiver_domain, &receiver_ep);
        }
        
        /* Clean up resources */
        if (receiver_ep.mr) fi_close(&receiver_ep.mr->fid);
        if (receiver_ep.buffer) free(receiver_ep.buffer);
        if (receiver_ep.address) free(receiver_ep.address);
        
        if (sender_ep.mr) fi_close(&sender_ep.mr->fid);
        if (sender_ep.buffer) free(sender_ep.buffer);
        if (sender_ep.address) free(sender_ep.address);
        
        /* Reset endpoint contexts */
        memset(&sender_ep, 0, sizeof(sender_ep));
        memset(&receiver_ep, 0, sizeof(receiver_ep));
        
        /* Progress indicator - show every 100 iterations like Python */
        if ((iteration + 1) % 100 == 0) {
            double elapsed = get_time_ms() - start_time;
            double rate = iteration / (elapsed / 1000.0);
            LOG_INFO("Progress: %d/%d iterations completed (%.1f iter/s, %.1f seconds elapsed)", 
                     iteration + 1, MAX_ITERATIONS, rate, elapsed / 1000.0);
        }
    }
    
    double total_elapsed = get_time_ms() - start_time;
    if (iteration == MAX_ITERATIONS) {
        LOG_INFO("✅ Test completed: %d iterations without triggering assertion", iteration);
        LOG_INFO("Total time: %.1f seconds (%.1f minutes)", total_elapsed / 1000.0, total_elapsed / 60000.0);
        LOG_INFO("Bug may be fixed or need different conditions to reproduce");
    } else {
        LOG_ERROR("❌ Test stopped after %d iterations due to errors", iteration);
        LOG_ERROR("Total time: %.1f seconds (%.1f minutes)", total_elapsed / 1000.0, total_elapsed / 60000.0);
        LOG_ERROR("If process aborted, check for assertion failure above");
    }
    
cleanup:
    /* Cleanup domains */
    if (receiver_domain.cq_thread) completion_thread_destroy(receiver_domain.cq_thread);
    if (receiver_domain.domain) fi_close(&receiver_domain.domain->fid);
    if (receiver_domain.fabric) fi_close(&receiver_domain.fabric->fid);
    if (receiver_domain.info) fi_freeinfo(receiver_domain.info);
    
    if (sender_domain.cq_thread) completion_thread_destroy(sender_domain.cq_thread);
    if (sender_domain.domain) fi_close(&sender_domain.domain->fid);
    if (sender_domain.fabric) fi_close(&sender_domain.fabric->fid);
    if (sender_domain.info) fi_freeinfo(sender_domain.info);
    
    return (errors > 0) ? 1 : 0;
}