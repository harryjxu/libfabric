#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>

/* Test configuration */
#define BUFFER_SIZE 512
#define MAX_ENDPOINTS 256  // Increase to handle many kill/recreate cycles
#define DEFAULT_ITERATIONS 100000
#define MAX_KILL_DELAY_MS 20
#define TRANSFER_TIMEOUT_MS 500

/* EFA-specific settings */
#define EFA_PROVIDER "efa"
#define FABRIC_VERSION FI_VERSION(1, 22)

/* Error handling macros */
#define CHECK_FI_ERROR(ret, msg) \
    do { \
        if (ret) { \
            fprintf(stderr, "%s:%d: %s failed: %s (%d)\n", \
                    __FILE__, __LINE__, msg, fi_strerror(-ret), ret); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

#define CHECK_ERROR(cond, msg) \
    do { \
        if (cond) { \
            fprintf(stderr, "%s:%d: %s: %s\n", \
                    __FILE__, __LINE__, msg, strerror(errno)); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

#define LOG_INFO(fmt, ...) \
    do { \
        printf("[INFO] " fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } while (0)

#define LOG_ERROR(fmt, ...) \
    do { \
        fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } while (0)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (getenv("DEBUG")) { \
            printf("[DEBUG] " fmt "\n", ##__VA_ARGS__); \
            fflush(stdout); \
        } \
    } while (0)

/* Timing utilities */
static inline double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static inline void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

/* Common structures */
struct endpoint_info {
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_ep *ep;
    struct fid_cq *cq;
    struct fid_av *av;
    struct fid_mr *mr;
    void *buffer;
    size_t buffer_size;
    fi_addr_t peer_addr;
    void *peer_address_raw;
    size_t peer_address_len;
};

/* Thread communication structures */
enum operation_type {
    OP_CREATE_ENDPOINT,
    OP_CLOSE_ENDPOINT,
    OP_TSEND,
    OP_TRECV,
    OP_SHUTDOWN
};

struct operation_request {
    enum operation_type type;
    union {
        struct {
            // For endpoint creation
            int response_fd;  // pipe to send response back
        } create_ep;
        struct {
            // For send/recv operations
            struct fid_ep *ep;
            void *buf;
            size_t len;
            void *desc;
            void *peer_addr;
            size_t peer_addr_len;
            uint64_t tag;
            uint64_t ignore;  // for trecv
            void *context;
            void (*completion_fn)(void *context, int status);
        } transfer;
    };
};

struct endpoint_response {
    struct fid_ep *ep;
    struct fid_av *av;
    void *address;
    size_t address_len;
};

/* Function to print fi_info details */
static inline void print_fi_info(struct fi_info *info) {
    LOG_INFO("Provider: %s", info->fabric_attr->prov_name);
    LOG_INFO("Fabric: %s", info->fabric_attr->name);
    LOG_INFO("Domain: %s", info->domain_attr->name);
    LOG_INFO("Mode: 0x%lx", info->mode);
    LOG_INFO("EP type: %d", info->ep_attr->type);
    LOG_INFO("Protocol: %d", info->ep_attr->protocol);
    
    if (info->src_addr) {
        char addr_str[256];
        size_t len = sizeof(addr_str);
        fi_av_straddr(NULL, info->src_addr, addr_str, &len);
        LOG_INFO("Source address: %s", addr_str);
    }
}

#endif /* COMMON_H */