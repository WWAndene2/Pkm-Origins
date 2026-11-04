/* The system services' requests (svc SendSyncRequest): the command buffer is the thread's local storage + 0x80; its
 * first word the header (command id << 16, normal words << 6, translate words), the reply in the same buffer, its
 * second word the result. A service is a table of commands (service/<Name>.c), each answering as Azahar's does; a
 * command not done stops the thread (fault 12: the service and command in the message), never a made-up reply.
 */
#ifndef HOS_IPC_H
#define HOS_IPC_H

#include "Kernel.h"

typedef struct HosRequest {
    HosThread* thread;
    HosObject* session;
    uint32_t buffer; /* the command buffer's game address */
    uint32_t command; /* the command id */
} HosRequest;

typedef void (*HosCommand)(HosRequest* q);

typedef struct {
    uint16_t id;
    HosCommand run;
    const char* name; /* Azahar's name for it */
} HosCommandEntry;

typedef struct HosService {
    const char* name; /* its port name ("fs:USER") */
    const HosCommandEntry* commands; /* ending with a NULL run */
} HosService;

/* the request's words: in(q, k) the k-th word of the buffer (0 the header) */
uint32_t hos_in(HosRequest* q, int k);
/* the reply: header for normal / translate words, then each word */
void hos_reply(HosRequest* q, int normal, int translate);
void hos_out(HosRequest* q, int k, uint32_t v);
/* a translate descriptor for n handles copied (moved) and the handles, from word k; the next word returned */
int hos_out_handles(HosRequest* q, int k, int move, const uint32_t* handles, int n);
/* a static buffer descriptor (id) and the reply's data written where the game's thread set its receiving buffer up
   (its local storage + 0x180 + 8 id: descriptor, address), from word k; the next word returned */
int hos_out_static(HosRequest* q, int k, int id, const void* data, uint32_t size);
/* a request's static buffer (descriptor at word k): its address and size */
uint32_t hos_in_static(HosRequest* q, int k, uint32_t* size);
/* a result code as Azahar's Result builds it (description, module, summary, level) */
#define HOS_RESULT(description, module, summary, level) \
    ((uint32_t)(description) | (uint32_t)(module) << 10 | (uint32_t)(summary) << 21 | (uint32_t)(level) << 27)

/* a session to a service, as a new handle */
uint32_t hos_session_open(const HosService* s);
const HosService* hos_service_find(const char* name);

enum { HOS_FAULT_SERVICE = 12 };

/* the services, one file each in service/ */
extern const HosService hos_srv, hos_apt_a, hos_apt_u, hos_ndm_u, hos_cfg_u, hos_ptm_u, hos_frd_u, hos_boss_u, hos_fs_user, hos_hid_user, hos_gsp_gpu, hos_dsp_dsp, hos_ldr_ro;

#endif
