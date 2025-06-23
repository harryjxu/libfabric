/*
 * endpoint_test.c - Test endpoint creation and destruction lifecycle
 * 
 * This program creates and destroys endpoints repeatedly to verify
 * proper cleanup and that no resources are leaked.
 */

#include "common.h"

/* Test parameters */
#define NUM_ITERATIONS 1000
#define NUM_ENDPOINTS 2

struct test_context {
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_av *av;
    struct endpoint_info endpoints[NUM_ENDPOINTS];
};

static int init_fabric(struct test_context *ctx) {
    struct fi_info *hints;
    int ret;
    
    /* Setup hints */
    hints = fi_allocinfo();
    CHECK_ERROR(!hints, "Failed to allocate hints");
    
    hints->fabric_attr->prov_name = strdup(EFA_PROVIDER);
    hints->ep_attr->type = FI_EP_RDM;
    hints->caps = FI_MSG | FI_TAGGED | FI_SEND | FI_RECV;
    hints->mode = 0;
    
    /* Get fabric info */
    ret = fi_getinfo(FABRIC_VERSION, NULL, NULL, 0, hints, &ctx->info);
    CHECK_FI_ERROR(ret, "fi_getinfo");
    
    /* Create fabric */
    ret = fi_fabric(ctx->info->fabric_attr, &ctx->fabric, NULL);
    CHECK_FI_ERROR(ret, "fi_fabric");
    
    /* Create domain */
    ret = fi_domain(ctx->fabric, ctx->info, &ctx->domain, NULL);
    CHECK_FI_ERROR(ret, "fi_domain");
    
    /* Create address vector */
    struct fi_av_attr av_attr = {0};
    av_attr.type = FI_AV_TABLE;
    av_attr.count = NUM_ENDPOINTS;
    
    ret = fi_av_open(ctx->domain, &av_attr, &ctx->av, NULL);
    CHECK_FI_ERROR(ret, "fi_av_open");
    
    fi_freeinfo(hints);
    return 0;
}

static int create_endpoint(struct test_context *ctx, struct endpoint_info *ep) {
    int ret;
    struct fi_cq_attr cq_attr = {0};
    
    /* Create completion queue */
    cq_attr.size = 128;
    cq_attr.format = FI_CQ_FORMAT_TAGGED;
    cq_attr.wait_obj = FI_WAIT_NONE;
    
    ret = fi_cq_open(ctx->domain, &cq_attr, &ep->cq, NULL);
    if (ret) {
        LOG_ERROR("fi_cq_open failed: %s (%d)", fi_strerror(-ret), ret);
        return ret;
    }
    
    /* Create endpoint */
    ret = fi_endpoint(ctx->domain, ctx->info, &ep->ep, NULL);
    CHECK_FI_ERROR(ret, "fi_endpoint");
    
    /* Bind CQ to endpoint */
    ret = fi_ep_bind(ep->ep, &ep->cq->fid, FI_TRANSMIT | FI_RECV);
    CHECK_FI_ERROR(ret, "fi_ep_bind cq");
    
    /* For EFA, check if we need to bind AV */
    LOG_DEBUG("Info mode: 0x%lx", ctx->info->mode);
    if (!(ctx->info->mode & FI_AV_USER_ID)) {
        /* Bind AV to endpoint only if not using user ID mode */
        ret = fi_ep_bind(ep->ep, &ctx->av->fid, 0);
        if (ret == -FI_ENOSYS) {
            /* Function not implemented is OK for EFA */
            LOG_DEBUG("AV bind not supported (expected for EFA)");
        } else if (ret) {
            LOG_ERROR("fi_ep_bind av failed: %s (%d)", fi_strerror(-ret), ret);
            return ret;
        }
    } else {
        LOG_DEBUG("Skipping AV bind due to FI_AV_USER_ID mode");
    }
    
    /* Enable endpoint */
    ret = fi_enable(ep->ep);
    CHECK_FI_ERROR(ret, "fi_enable");
    
    /* Allocate buffer */
    ep->buffer_size = BUFFER_SIZE;
    ep->buffer = malloc(ep->buffer_size);
    CHECK_ERROR(!ep->buffer, "Failed to allocate buffer");
    
    /* Register memory */
    ret = fi_mr_reg(ctx->domain, ep->buffer, ep->buffer_size,
                    FI_SEND | FI_RECV, 0, 0, 0, &ep->mr, NULL);
    CHECK_FI_ERROR(ret, "fi_mr_reg");
    
    /* Get endpoint address */
    size_t addrlen = 256;
    ep->peer_address_raw = malloc(addrlen);
    CHECK_ERROR(!ep->peer_address_raw, "Failed to allocate address buffer");
    
    ret = fi_getname(&ep->ep->fid, ep->peer_address_raw, &addrlen);
    CHECK_FI_ERROR(ret, "fi_getname");
    ep->peer_address_len = addrlen;
    
    return 0;
}

static void destroy_endpoint(struct endpoint_info *ep) {
    /* Close in reverse order of creation */
    if (ep->mr) {
        fi_close(&ep->mr->fid);
        ep->mr = NULL;
    }
    
    if (ep->buffer) {
        free(ep->buffer);
        ep->buffer = NULL;
    }
    
    if (ep->peer_address_raw) {
        free(ep->peer_address_raw);
        ep->peer_address_raw = NULL;
    }
    
    if (ep->ep) {
        fi_close(&ep->ep->fid);
        ep->ep = NULL;
    }
    
    if (ep->cq) {
        fi_close(&ep->cq->fid);
        ep->cq = NULL;
    }
}

static void cleanup_fabric(struct test_context *ctx) {
    if (ctx->av) {
        fi_close(&ctx->av->fid);
        ctx->av = NULL;
    }
    
    if (ctx->domain) {
        fi_close(&ctx->domain->fid);
        ctx->domain = NULL;
    }
    
    if (ctx->fabric) {
        fi_close(&ctx->fabric->fid);
        ctx->fabric = NULL;
    }
    
    if (ctx->info) {
        fi_freeinfo(ctx->info);
        ctx->info = NULL;
    }
}

static int run_iteration(struct test_context *ctx, int iter) {
    int ret = 0;
    int i;
    double start_time, create_time, destroy_time;
    
    LOG_DEBUG("Iteration %d: Creating %d endpoints", iter, NUM_ENDPOINTS);
    
    /* Create all endpoints */
    start_time = get_time_ms();
    for (i = 0; i < NUM_ENDPOINTS; i++) {
        ret = create_endpoint(ctx, &ctx->endpoints[i]);
        if (ret) {
            LOG_ERROR("Failed to create endpoint %d in iteration %d", i, iter);
            goto cleanup;
        }
    }
    create_time = get_time_ms() - start_time;
    
    /* Exchange addresses between endpoints */
    fi_addr_t addrs[NUM_ENDPOINTS];
    for (i = 0; i < NUM_ENDPOINTS; i++) {
        LOG_DEBUG("Inserting address %d (len=%zu)", i, ctx->endpoints[i].peer_address_len);
        ret = fi_av_insert(ctx->av, ctx->endpoints[i].peer_address_raw, 
                          1, &addrs[i], 0, NULL);
        if (ret < 0) {
            LOG_ERROR("fi_av_insert failed: %s (%d)", fi_strerror(-ret), ret);
            ret = -1;
            goto cleanup;
        } else if (ret != 1) {
            LOG_ERROR("fi_av_insert inserted %d addresses, expected 1", ret);
            ret = -1;
            goto cleanup;
        }
        ctx->endpoints[i].peer_addr = addrs[i];
        LOG_DEBUG("Address %d inserted successfully, fi_addr=0x%lx", i, (unsigned long)addrs[i]);
    }
    
    /* Small delay to simulate some work */
    sleep_ms(1);
    
    /* If we got here without errors, set ret to success */
    if (ret >= 0) {
        ret = 0;
    }
    
    /* Destroy all endpoints */
    start_time = get_time_ms();
cleanup:
    for (i = NUM_ENDPOINTS - 1; i >= 0; i--) {
        destroy_endpoint(&ctx->endpoints[i]);
    }
    destroy_time = get_time_ms() - start_time;
    
    LOG_DEBUG("  Create time: %.2f ms, Destroy time: %.2f ms", 
              create_time, destroy_time);
    
    return ret;
}

int main(void) {
    struct test_context ctx = {0};
    int ret;
    int i;
    double total_start, total_time;
    int success_count = 0;
    int fail_count = 0;
    
    LOG_INFO("Endpoint Lifecycle Test");
    LOG_INFO("Testing %d iterations of %d endpoints each", 
             NUM_ITERATIONS, NUM_ENDPOINTS);
    
    /* Initialize fabric */
    ret = init_fabric(&ctx);
    if (ret) {
        LOG_ERROR("Failed to initialize fabric");
        return EXIT_FAILURE;
    }
    
    LOG_INFO("Fabric initialized successfully");
    LOG_INFO("Provider: %s", ctx.info->fabric_attr->prov_name);
    LOG_INFO("Domain: %s", ctx.info->domain_attr->name);
    
    /* Run test iterations */
    total_start = get_time_ms();
    
    for (i = 0; i < NUM_ITERATIONS; i++) {
        ret = run_iteration(&ctx, i);
        if (ret) {
            fail_count++;
            LOG_ERROR("Iteration %d failed", i);
            /* Continue testing to see if it's consistent */
        } else {
            success_count++;
        }
        
        /* Progress indicator every 100 iterations */
        if ((i + 1) % 100 == 0) {
            LOG_INFO("Progress: %d/%d iterations complete", i + 1, NUM_ITERATIONS);
        }
    }
    
    total_time = get_time_ms() - total_start;
    
    /* Print results */
    LOG_INFO("\n=== Test Results ===");
    LOG_INFO("Total iterations: %d", NUM_ITERATIONS);
    LOG_INFO("Successful: %d", success_count);
    LOG_INFO("Failed: %d", fail_count);
    LOG_INFO("Total time: %.2f seconds", total_time / 1000.0);
    LOG_INFO("Average time per iteration: %.2f ms", total_time / NUM_ITERATIONS);
    
    if (fail_count == 0) {
        LOG_INFO("\n✅ SUCCESS: All endpoint lifecycle tests passed!");
    } else {
        LOG_ERROR("\n❌ FAILED: %d iterations failed", fail_count);
    }
    
    /* Cleanup */
    cleanup_fabric(&ctx);
    
    return (fail_count == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}