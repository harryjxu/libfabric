#ifndef COMPLETION_THREAD_H
#define COMPLETION_THREAD_H

#include "common.h"
#include <stdbool.h>

/* Address cache entry */
struct addr_cache_entry {
    uint8_t addr[256];
    size_t addr_len;
    fi_addr_t fi_addr;
    struct addr_cache_entry *next;
};

/* Wrapper for completion context */
struct completion_wrapper {
    void *user_context;
    void (*completion_fn)(void *context, int status);
};

/* Completion thread context */
struct completion_thread {
    pthread_t thread_id;
    struct fid_domain *domain;
    struct fid_cq *cq;
    int cmd_pipe[2];  // For receiving commands
    bool running;
    
    /* Current endpoint - only one active endpoint per domain at a time */
    struct fid_ep *current_ep;
    struct fid_av *current_av;
    struct addr_cache_entry *addr_cache;
};

/* Create and start a completion thread for a domain */
struct completion_thread *completion_thread_create(struct fid_domain *domain);

/* Send operation to completion thread */
int completion_thread_send_op(struct completion_thread *thread, struct operation_request *req);

/* Stop and cleanup completion thread */
void completion_thread_destroy(struct completion_thread *thread);

#endif /* COMPLETION_THREAD_H */