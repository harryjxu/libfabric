/*
 * completion_thread.c - Worker thread for handling libfabric completions
 * 
 * This implements the same threading model as fabulous where each domain
 * has a dedicated completion thread that handles all operations.
 */

#include "completion_thread.h"
#include <sched.h>

/* Find or create address entry in cache */
static fi_addr_t get_or_insert_address(struct completion_thread *ctx, 
                                       struct fid_av *av,
                                       void *addr, size_t addr_len) {
    /* Check if this is for the current AV */
    if (av != ctx->current_av) {
        LOG_ERROR("AV mismatch - not the current AV");
        return FI_ADDR_UNSPEC;
    }
    
    /* Check cache first */
    struct addr_cache_entry *entry = ctx->addr_cache;
    while (entry) {
        if (entry->addr_len == addr_len && 
            memcmp(entry->addr, addr, addr_len) == 0) {
            return entry->fi_addr;
        }
        entry = entry->next;
    }
    
    /* Not in cache, insert it */
    fi_addr_t fi_addr;
    int ret = fi_av_insert(av, addr, 1, &fi_addr, 0, NULL);
    if (ret < 0) {
        LOG_ERROR("fi_av_insert failed: %s (%d)", fi_strerror(-ret), ret);
        return FI_ADDR_UNSPEC;
    }
    if (ret != 1) {
        LOG_ERROR("fi_av_insert inserted %d addresses, expected 1", ret);
        return FI_ADDR_UNSPEC;
    }
    
    /* Add to cache */
    entry = malloc(sizeof(*entry));
    if (!entry) {
        LOG_ERROR("Failed to allocate address cache entry");
        return fi_addr;
    }
    
    memcpy(entry->addr, addr, addr_len);
    entry->addr_len = addr_len;
    entry->fi_addr = fi_addr;
    entry->next = ctx->addr_cache;
    ctx->addr_cache = entry;
    
    LOG_DEBUG("Cached address %p (len=%zu) -> fi_addr=0x%lx", addr, addr_len, fi_addr);
    return fi_addr;
}

/* Handle endpoint creation */
static void handle_create_endpoint(struct completion_thread *ctx, 
                                  struct operation_request *req) {
    struct endpoint_response resp = {0};
    int ret;
    
    /* Close any existing endpoint first */
    if (ctx->current_ep) {
        LOG_DEBUG("Closing existing endpoint before creating new one");
        fi_close(&ctx->current_ep->fid);
        ctx->current_ep = NULL;
    }
    
    if (ctx->current_av) {
        fi_close(&ctx->current_av->fid);
        ctx->current_av = NULL;
    }
    
    /* Free address cache */
    while (ctx->addr_cache) {
        struct addr_cache_entry *next = ctx->addr_cache->next;
        free(ctx->addr_cache);
        ctx->addr_cache = next;
    }
    
    /* Create endpoint - matching fabulous sequence exactly */
    struct fi_info *info = fi_allocinfo();
    if (!info) {
        LOG_ERROR("Failed to allocate fi_info");
        goto send_response;
    }
    
    info->fabric_attr->prov_name = strdup(EFA_PROVIDER);
    info->ep_attr->type = FI_EP_RDM;
    info->caps = FI_MSG | FI_TAGGED | FI_SEND | FI_RECV;
    
    ret = fi_getinfo(FABRIC_VERSION, NULL, NULL, 0, info, &info);
    if (ret) {
        LOG_ERROR("fi_getinfo failed: %s", fi_strerror(-ret));
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Create endpoint */
    struct fid_ep *ep = NULL;
    ret = fi_endpoint(ctx->domain, info, &ep, NULL);
    if (ret) {
        LOG_ERROR("fi_endpoint failed: %s", fi_strerror(-ret));
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Create address vector */
    struct fid_av *av = NULL;
    struct fi_av_attr av_attr = {
        .type = FI_AV_TABLE,
        .count = 1024,  // Match fabulous AV_SIZE
    };
    
    ret = fi_av_open(ctx->domain, &av_attr, &av, NULL);
    if (ret) {
        LOG_ERROR("fi_av_open failed: %s", fi_strerror(-ret));
        fi_close(&ep->fid);
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Optional delay for debugging (matches FABULOUS_EP_BIND_DELAY_MS) */
    const char *delay_env = getenv("FABULOUS_EP_BIND_DELAY_MS");
    if (delay_env) {
        int delay_ms = atoi(delay_env);
        if (delay_ms > 0) {
            LOG_DEBUG("Delaying endpoint bind by %d ms", delay_ms);
            sleep_ms(delay_ms);
        }
    }
    
    /* Bind AV to endpoint */
    ret = fi_ep_bind(ep, &av->fid, 0);
    if (ret) {
        LOG_ERROR("fi_ep_bind(av) failed: %s", fi_strerror(-ret));
        fi_close(&av->fid);
        fi_close(&ep->fid);
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Bind CQ to endpoint */
    ret = fi_ep_bind(ep, &ctx->cq->fid, FI_RECV | FI_TRANSMIT);
    if (ret) {
        LOG_ERROR("fi_ep_bind(cq) failed: %s", fi_strerror(-ret));
        fi_close(&av->fid);
        fi_close(&ep->fid);
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Enable endpoint */
    ret = fi_enable(ep);
    if (ret) {
        LOG_ERROR("fi_enable failed: %s", fi_strerror(-ret));
        fi_close(&av->fid);
        fi_close(&ep->fid);
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Get endpoint address */
    uint8_t addr_buf[256];
    size_t addr_len = sizeof(addr_buf);
    ret = fi_getname(&ep->fid, addr_buf, &addr_len);
    if (ret) {
        LOG_ERROR("fi_getname failed: %s", fi_strerror(-ret));
        fi_close(&av->fid);
        fi_close(&ep->fid);
        fi_freeinfo(info);
        goto send_response;
    }
    
    /* Success - save endpoint info */
    ctx->current_ep = ep;
    ctx->current_av = av;
    ctx->addr_cache = NULL;
    
    /* Prepare response */
    resp.ep = ep;
    resp.av = av;
    resp.address = malloc(addr_len);
    if (resp.address) {
        memcpy(resp.address, addr_buf, addr_len);
        resp.address_len = addr_len;
    }
    
    fi_freeinfo(info);
    LOG_DEBUG("Created endpoint %p with AV %p", ep, av);
    
send_response:
    /* Send response back to main thread */
    if (write(req->create_ep.response_fd, &resp, sizeof(resp)) != sizeof(resp)) {
        LOG_ERROR("Failed to send endpoint response");
        if (resp.address) free(resp.address);
    }
}

/* Handle send operation */
static void handle_tsend(struct completion_thread *ctx, struct operation_request *req) {
    assert(ctx != NULL && "handle_tsend: ctx is NULL");
    assert(req != NULL && "handle_tsend: req is NULL");
    assert(req->transfer.ep != NULL && "handle_tsend: endpoint is NULL");
    
    fi_addr_t peer_id = FI_ADDR_UNSPEC;
    
    /* Check if this is for the current endpoint */
    if (req->transfer.ep != ctx->current_ep) {
        LOG_ERROR("Endpoint mismatch - not the current endpoint");
        assert(0 && "Endpoint mismatch in handle_tsend");
        if (req->transfer.completion_fn) {
            req->transfer.completion_fn(req->transfer.context, -FI_EINVAL);
        }
        return;
    }
    
    struct fid_av *av = ctx->current_av;
    if (!av) {
        LOG_ERROR("No current AV");
        assert(0 && "No current AV in handle_tsend");
        if (req->transfer.completion_fn) {
            req->transfer.completion_fn(req->transfer.context, -FI_ENOENT);
        }
        return;
    }
    
    /* Get or insert peer address */
    if (req->transfer.peer_addr && req->transfer.peer_addr_len > 0) {
        peer_id = get_or_insert_address(ctx, av, 
                                       req->transfer.peer_addr, 
                                       req->transfer.peer_addr_len);
        if (peer_id == FI_ADDR_UNSPEC) {
            LOG_ERROR("Failed to get peer address");
            if (req->transfer.completion_fn) {
                req->transfer.completion_fn(req->transfer.context, -FI_EINVAL);
            }
            return;
        }
    }
    
    /* Create completion wrapper - this is what libfabric will give us back */
    struct completion_wrapper *wrapper = malloc(sizeof(struct completion_wrapper));
    if (!wrapper) {
        LOG_ERROR("Failed to allocate completion wrapper");
        assert(0 && "Failed to allocate completion wrapper");
        if (req->transfer.completion_fn) {
            req->transfer.completion_fn(req->transfer.context, -ENOMEM);
        }
        return;
    }
    assert(wrapper != NULL && "wrapper allocation check");
    assert(req->transfer.context != NULL && "transfer context is NULL");
    assert(req->transfer.completion_fn != NULL && "completion_fn is NULL");
    
    wrapper->user_context = req->transfer.context;
    wrapper->completion_fn = req->transfer.completion_fn;
    
    /* Submit send - retry on EAGAIN like fabulous does */
    int ret;
    while ((ret = fi_tsend(req->transfer.ep, req->transfer.buf, req->transfer.len,
                          req->transfer.desc, peer_id, req->transfer.tag,
                          wrapper)) == -FI_EAGAIN) {
        /* Poll CQ to make progress */
        struct fi_cq_data_entry entry;
        fi_cq_read(ctx->cq, &entry, 1);
        sched_yield();
    }
    
    if (ret) {
        LOG_ERROR("fi_tsend failed: %s (%d)", fi_strerror(-ret), ret);
        if (wrapper->completion_fn) {
            wrapper->completion_fn(wrapper->user_context, ret);
        }
        free(wrapper);
    }
}

/* Handle receive operation */
static void handle_trecv(struct completion_thread *ctx, struct operation_request *req) {
    assert(ctx != NULL && "handle_trecv: ctx is NULL");
    assert(req != NULL && "handle_trecv: req is NULL");
    assert(req->transfer.ep != NULL && "handle_trecv: endpoint is NULL");
    
    fi_addr_t peer_id = FI_ADDR_UNSPEC;
    
    /* Check if this is for the current endpoint */
    if (req->transfer.ep != ctx->current_ep) {
        LOG_ERROR("Endpoint mismatch - not the current endpoint");
        assert(0 && "Endpoint mismatch in handle_trecv");
        if (req->transfer.completion_fn) {
            req->transfer.completion_fn(req->transfer.context, -FI_EINVAL);
        }
        return;
    }
    
    struct fid_av *av = ctx->current_av;
    if (!av) {
        LOG_ERROR("No current AV");
        assert(0 && "No current AV in handle_trecv");
        if (req->transfer.completion_fn) {
            req->transfer.completion_fn(req->transfer.context, -FI_ENOENT);
        }
        return;
    }
    
    /* Get or insert peer address if specified */
    if (req->transfer.peer_addr && req->transfer.peer_addr_len > 0) {
        peer_id = get_or_insert_address(ctx, av, 
                                       req->transfer.peer_addr, 
                                       req->transfer.peer_addr_len);
        if (peer_id == FI_ADDR_UNSPEC) {
            LOG_ERROR("Failed to get peer address");
            if (req->transfer.completion_fn) {
                req->transfer.completion_fn(req->transfer.context, -FI_EINVAL);
            }
            return;
        }
    }
    
    /* Create completion wrapper - this is what libfabric will give us back */
    struct completion_wrapper *wrapper = malloc(sizeof(struct completion_wrapper));
    if (!wrapper) {
        LOG_ERROR("Failed to allocate completion wrapper");
        assert(0 && "Failed to allocate completion wrapper");
        if (req->transfer.completion_fn) {
            req->transfer.completion_fn(req->transfer.context, -ENOMEM);
        }
        return;
    }
    assert(wrapper != NULL && "wrapper allocation check");
    assert(req->transfer.context != NULL && "transfer context is NULL");
    assert(req->transfer.completion_fn != NULL && "completion_fn is NULL");
    
    wrapper->user_context = req->transfer.context;
    wrapper->completion_fn = req->transfer.completion_fn;
    
    /* Submit receive - retry on EAGAIN like fabulous does */
    int ret;
    while ((ret = fi_trecv(req->transfer.ep, req->transfer.buf, req->transfer.len,
                          req->transfer.desc, peer_id, req->transfer.tag,
                          req->transfer.ignore, wrapper)) == -FI_EAGAIN) {
        /* Poll CQ to make progress */
        struct fi_cq_data_entry entry;
        fi_cq_read(ctx->cq, &entry, 1);
        sched_yield();
    }
    
    if (ret) {
        LOG_ERROR("fi_trecv failed: %s (%d)", fi_strerror(-ret), ret);
        if (wrapper->completion_fn) {
            wrapper->completion_fn(wrapper->user_context, ret);
        }
        free(wrapper);
    }
}

/* Handle endpoint close - matching fabulous exactly */
static void handle_close_endpoint(struct completion_thread *ctx, struct operation_request *req) {
    struct fid_ep *ep = req->transfer.ep;
    
    LOG_DEBUG("Closing endpoint %p in completion thread", ep);
    
    /* Check if this is the current endpoint */
    if (ep != ctx->current_ep) {
        LOG_ERROR("Endpoint %p is not the current endpoint %p", ep, ctx->current_ep);
        return;
    }
    
    /* Save AV reference before clearing */
    struct fid_av *av = ctx->current_av;
    
    /* Clear references FIRST to prevent any new operations */
    ctx->current_ep = NULL;
    ctx->current_av = NULL;
    
    /* Now close endpoint */
    if (ep) {
        fi_close(&ep->fid);
    }
    
    /* Then close AV if it exists */
    if (av) {
        fi_close(&av->fid);
    }
    
    /* Finally free address cache */
    while (ctx->addr_cache) {
        struct addr_cache_entry *next = ctx->addr_cache->next;
        free(ctx->addr_cache);
        ctx->addr_cache = next;
    }
    
    LOG_DEBUG("Endpoint and AV closed successfully");
}

/* Poll completion queue */
static void poll_cq(struct completion_thread *ctx) {
    assert(ctx != NULL && "poll_cq: ctx is NULL");
    assert(ctx->cq != NULL && "poll_cq: ctx->cq is NULL");
    
    struct fi_cq_data_entry entry;
    int ret = fi_cq_read(ctx->cq, &entry, 1);
    
    if (ret == 1) {
        /* Success completion */
        struct completion_wrapper *wrapper = (struct completion_wrapper *)entry.op_context;
        if (wrapper) {
            assert(wrapper->completion_fn != NULL && "poll_cq: wrapper->completion_fn is NULL");
            assert(wrapper->user_context != NULL && "poll_cq: wrapper->user_context is NULL");
            if (wrapper->completion_fn) {
                wrapper->completion_fn(wrapper->user_context, 0);
            }
            free(wrapper);
        } else {
            LOG_ERROR("WARNING: Got completion with NULL wrapper!");
        }
    } else if (ret == -FI_EAVAIL) {
        /* Error completion */
        struct fi_cq_err_entry err_entry;
        memset(&err_entry, 0, sizeof(err_entry));
        
        /* Allocate error data buffer - libfabric may write up to 1024 bytes */
        uint8_t err_data_buf[1024];
        err_entry.err_data = err_data_buf;
        err_entry.err_data_size = sizeof(err_data_buf);
        
        ret = fi_cq_readerr(ctx->cq, &err_entry, 0);
        if (ret >= 0) {
            struct completion_wrapper *wrapper = (struct completion_wrapper *)err_entry.op_context;
            
            LOG_ERROR("CQ error: %s (provider: %s)",
                     fi_strerror(err_entry.err),
                     fi_cq_strerror(ctx->cq, err_entry.prov_errno, 
                                   err_entry.err_data, NULL, 0));
            
            if (wrapper) {
                assert(wrapper->completion_fn != NULL && "poll_cq error: wrapper->completion_fn is NULL");
                assert(wrapper->user_context != NULL && "poll_cq error: wrapper->user_context is NULL");
                if (wrapper->completion_fn) {
                    wrapper->completion_fn(wrapper->user_context, -err_entry.err);
                }
                free(wrapper);
            } else {
                LOG_ERROR("WARNING: Got error completion with NULL wrapper!");
            }
        }
    }
}

/* Worker thread main loop */
static void *worker_thread(void *arg) {
    struct completion_thread *ctx = arg;
    fd_set readfds;
    struct timeval tv;
    
    LOG_INFO("Completion thread started for domain %p", ctx->domain);
    
    while (ctx->running) {
        /* Check for commands with short timeout */
        FD_ZERO(&readfds);
        FD_SET(ctx->cmd_pipe[0], &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 1ms timeout
        
        int ret = select(ctx->cmd_pipe[0] + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(ctx->cmd_pipe[0], &readfds)) {
            /* Read command */
            struct operation_request req;
            ssize_t n = read(ctx->cmd_pipe[0], &req, sizeof(req));
            if (n != sizeof(req)) {
                LOG_ERROR("Failed to read complete operation request");
                continue;
            }
            
            /* Handle operation */
            switch (req.type) {
                case OP_CREATE_ENDPOINT:
                    handle_create_endpoint(ctx, &req);
                    break;
                case OP_TSEND:
                    handle_tsend(ctx, &req);
                    break;
                case OP_TRECV:
                    handle_trecv(ctx, &req);
                    break;
                case OP_CLOSE_ENDPOINT:
                    handle_close_endpoint(ctx, &req);
                    break;
                case OP_SHUTDOWN:
                    ctx->running = false;
                    break;
                default:
                    LOG_ERROR("Unknown operation type: %d", req.type);
            }
        }
        
        /* Always poll CQ like fabulous does */
        poll_cq(ctx);
        
        /* CPU hint for spinning */
        sched_yield();
    }
    
    LOG_INFO("Completion thread exiting");
    return NULL;
}

/* Create and start completion thread */
struct completion_thread *completion_thread_create(struct fid_domain *domain) {
    struct completion_thread *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        LOG_ERROR("Failed to allocate completion thread context");
        return NULL;
    }
    
    ctx->domain = domain;
    ctx->running = true;
    
    /* Create CQ matching fabulous settings */
    struct fi_cq_attr cq_attr = {
        .size = 1024,
        .format = FI_CQ_FORMAT_DATA,
        .wait_obj = FI_WAIT_NONE,  // EFA only supports this
    };
    
    int ret = fi_cq_open(domain, &cq_attr, &ctx->cq, NULL);
    if (ret) {
        LOG_ERROR("fi_cq_open failed: %s", fi_strerror(-ret));
        free(ctx);
        return NULL;
    }
    
    /* Create command pipe */
    if (pipe(ctx->cmd_pipe) < 0) {
        LOG_ERROR("Failed to create command pipe");
        fi_close(&ctx->cq->fid);
        free(ctx);
        return NULL;
    }
    
    /* Start worker thread */
    ret = pthread_create(&ctx->thread_id, NULL, worker_thread, ctx);
    if (ret) {
        LOG_ERROR("Failed to create worker thread");
        close(ctx->cmd_pipe[0]);
        close(ctx->cmd_pipe[1]);
        fi_close(&ctx->cq->fid);
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

/* Send operation to completion thread */
int completion_thread_send_op(struct completion_thread *thread, struct operation_request *req) {
    ssize_t n = write(thread->cmd_pipe[1], req, sizeof(*req));
    if (n != sizeof(*req)) {
        LOG_ERROR("Failed to send operation to completion thread");
        return -1;
    }
    return 0;
}

/* Stop and cleanup completion thread */
void completion_thread_destroy(struct completion_thread *thread) {
    if (!thread) return;
    
    /* Send shutdown command */
    struct operation_request req = {
        .type = OP_SHUTDOWN
    };
    completion_thread_send_op(thread, &req);
    
    /* Wait for thread to exit */
    pthread_join(thread->thread_id, NULL);
    
    /* Cleanup current endpoint */
    if (thread->current_ep) {
        fi_close(&thread->current_ep->fid);
    }
    if (thread->current_av) {
        fi_close(&thread->current_av->fid);
    }
    
    /* Free address cache */
    struct addr_cache_entry *entry = thread->addr_cache;
    while (entry) {
        struct addr_cache_entry *next = entry->next;
        free(entry);
        entry = next;
    }
    
    /* Cleanup resources */
    close(thread->cmd_pipe[0]);
    close(thread->cmd_pipe[1]);
    fi_close(&thread->cq->fid);
    free(thread);
}