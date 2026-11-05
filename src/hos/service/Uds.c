/* nwm::UDS: the local wireless (the Player Search System's neighbours), as Azahar answers it (service/nwm/nwm_uds.cpp)
 * with no network around: the game can host a network (it stays its only member: no beacon is sent, no packet comes),
 * bind its data channels, pull packets (none), and read its status and nodes. Layouts from Azahar's nwm_uds.h:
 * NodeInfo 0x28 bytes (the network node id at 0x20), NetworkInfo 0x108 (channel 6, OUI 0xC-0xF, total nodes 0x1C,
 * max nodes 0x1D, application data size 0x3F and data 0x40), ConnectionStatus 0x30 (status 0, reason 4, node id 8,
 * changed nodes 0xA, nodes 0xC, total 0x2C, max 0x2D, bitmask 0x2E). Other commands are added as the game calls them;
 * one not here stops the thread and names itself (Ipc.c). */
#include "../Ipc.h"

#include <string.h>

#include "../Memory.h"

enum { NOT_CONNECTED = 3, CONNECTED_AS_HOST = 6, CONNECTION_ESTABLISHED = 1, DEFAULT_CHANNEL = 11, MODULE_UDS = 68 };

static HosObject* status_event; /* (Azahar's connection_status_event) */
static uint8_t status[0x30], network[0x108], current_node[0x28], nodes[16][0x28];
static int node_count, initialized;
static uint8_t channel;
static struct { uint32_t bind_id; uint8_t data_channel; HosObject* event; } binds[16];

static void w16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

static void signal_event(HosObject* e)
{
    if (e) {
        e->signaled = 1;
        hos_object_wake(e);
    }
}

static void result(HosRequest* q, uint32_t r)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, r);
}

static void ok(HosRequest* q)
{
    result(q, HOS_OK);
}

static void initialize_with_version(HosRequest* q)
{
    if (!status_event)
        status_event = hos_object_new(HOS_EVENT);
    for (int k = 0; k < 10; k++)
        w32(current_node + 4 * k, hos_in(q, 2 + k));
    memset(status, 0, sizeof status);
    w32(status, NOT_CONNECTED);
    memcpy(nodes[0], current_node, sizeof current_node);
    node_count = 1;
    memset(binds, 0, sizeof binds);
    initialized = 1;
    uint32_t h = hos_handle_new(status_event);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, &h, 1);
}

static void get_connection_status(HosRequest* q)
{
    hos_reply(q, 13, 0);
    hos_out(q, 1, HOS_OK);
    for (int k = 0; k < 12; k++) {
        uint32_t v;
        memcpy(&v, status + 4 * k, 4);
        hos_out(q, 2 + k, v);
    }
    w16(status + 0xA, 0); /* (changed nodes: reported once) */
}

static void begin_hosting_network(HosRequest* q)
{
    uint32_t size;
    uint32_t at = hos_in_static(q, 2, &size);
    memset(network, 0, sizeof network);
    memcpy(network, hos_pointer(at), size < sizeof network ? size : sizeof network);
    uint8_t max_nodes = network[0x1D];
    memset(status, 0, sizeof status);
    w32(status, CONNECTED_AS_HOST);
    w32(status + 4, CONNECTION_ESTABLISHED);
    network[0xC] = 0x00, network[0xD] = 0x1F, network[0xE] = 0x32; /* (Nintendo's OUI) */
    network[0xF] = 21;                                                /* (its network information tag) */
    status[0x2D] = max_nodes;
    status[0x2C] = network[0x1C] = 1;
    w16(status + 8, 1);
    w16(current_node + 0x20, 1);
    w16(status + 0xC, 1);
    w16(status + 0x2E, 1);
    w16(status + 0xA, 1);
    /* the console's MAC address: Azahar's comes from its settings (a random one made once); here a fixed one with
       Nintendo's prefix, the network having no other member to tell it apart */
    static const uint8_t mac[6] = {0x40, 0xF4, 0x07, 0x00, 0x00, 0x01};
    memcpy(network, mac, 6);
    memset(nodes, 0, sizeof nodes);
    memcpy(nodes[0], current_node, sizeof current_node);
    node_count = max_nodes < 16 ? max_nodes : 16;
    if (network[6])
        channel = network[6];
    else
        network[6] = channel = DEFAULT_CHANNEL;
    signal_event(status_event);
    ok(q);
}

static void destroy_network(HosRequest* q)
{
    uint32_t s;
    memcpy(&s, status, 4);
    if (s != CONNECTED_AS_HOST) {
        result(q, HOS_RESULT(490, MODULE_UDS, 5, 25)); /* (Azahar's WrongStatus, InvalidState) */
        return;
    }
    uint16_t id;
    memcpy(&id, status + 8, 2);
    memset(status, 0, sizeof status);
    w32(status, NOT_CONNECTED);
    w16(status + 8, id);
    signal_event(status_event);
    for (int i = 0; i < 16; i++) {
        signal_event(binds[i].event);
        binds[i].bind_id = 0;
    }
    ok(q);
}

static void get_node_information(HosRequest* q)
{
    uint16_t id = (uint16_t)hos_in(q, 1);
    if (!initialized) {
        result(q, HOS_RESULT(1016, MODULE_UDS, 10, 25)); /* (NotInitialized, StatusChanged) */
        return;
    }
    for (int i = 0; i < node_count; i++) {
        uint16_t nid;
        memcpy(&nid, nodes[i] + 0x20, 2);
        if (nid == id) {
            hos_reply(q, 11, 0);
            hos_out(q, 1, HOS_OK);
            for (int k = 0; k < 10; k++) {
                uint32_t v;
                memcpy(&v, nodes[i] + 4 * k, 4);
                hos_out(q, 2 + k, v);
            }
            return;
        }
    }
    result(q, HOS_RESULT(1018, MODULE_UDS, 8, 25)); /* (NotFound, WrongArgument) */
}

static void set_application_data(HosRequest* q)
{
    uint32_t size = hos_in(q, 1), bytes;
    uint32_t at = hos_in_static(q, 2, &bytes);
    if (size > 0xC8) {
        result(q, HOS_RESULT(1001, MODULE_UDS, 8, 28)); /* (TooLarge) */
        return;
    }
    network[0x3F] = (uint8_t)size;
    memcpy(network + 0x40, hos_pointer(at), size);
    ok(q);
}

static void bind(HosRequest* q)
{
    uint32_t id = hos_in(q, 1), recv_size = hos_in(q, 2);
    uint8_t data_channel = (uint8_t)hos_in(q, 3);
    if (!data_channel || !id) {
        result(q, HOS_RESULT(1002, MODULE_UDS, 8, 28)); /* (NotAuthorized, WrongArgument, Usage) */
        return;
    }
    if (recv_size < 0x5F4) {
        result(q, HOS_RESULT(1001, MODULE_UDS, 8, 28));
        return;
    }
    for (int i = 0; i < 16; i++)
        if (!binds[i].bind_id) {
            binds[i].bind_id = id;
            binds[i].data_channel = data_channel;
            binds[i].event = hos_object_new(HOS_EVENT);
            uint32_t h = hos_handle_new(binds[i].event);
            hos_reply(q, 1, 2);
            hos_out(q, 1, HOS_OK);
            hos_out_handles(q, 2, 0, &h, 1);
            return;
        }
    result(q, HOS_RESULT(1011, MODULE_UDS, 3, 25)); /* (OutOfMemory, OutOfResource) */
}

static void pull_packet(HosRequest* q)
{
    uint32_t id = hos_in(q, 1), aligned = hos_in(q, 2);
    uint32_t s;
    memcpy(&s, status, 4);
    if (s != CONNECTED_AS_HOST && s != 9 && s != 10) {
        result(q, HOS_RESULT(1002, MODULE_UDS, 5, 25)); /* (NotAuthorized, InvalidState) */
        return;
    }
    int found = 0;
    for (int i = 0; i < 16; i++)
        found |= binds[i].bind_id == id;
    if (!found) {
        result(q, HOS_RESULT(1002, MODULE_UDS, 5, 25));
        return;
    }
    static uint8_t empty[0x172 << 2];
    uint32_t size = (aligned < 0x172 ? aligned : 0x172) << 2; /* (no packet: a zeroed buffer, size 0) */
    hos_reply(q, 3, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 0);
    hos_out(q, 3, 0);
    hos_out_static(q, 4, 0, empty, size);
}

/* the beacons of the networks around (none): the reply header (the buffer's size, the size written, 0 entries) in the
   mapped buffer, then the caller sleeps 300 ms as Azahar has it (UDSBeaconScanInterval) */
static void recv_beacon_broadcast_data(HosRequest* q)
{
    uint32_t size = hos_in(q, 1), desc = hos_in(q, 19), at = hos_in(q, 20);
    uint8_t* out = hos_pointer(at);
    w32(out, size);
    w32(out + 4, 12);
    w32(out + 8, 0);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, desc);
    hos_out(q, 3, at);
    static HosObject* never; /* (a wait only the timeout ends, as SleepThread's) */
    if (!never)
        never = hos_object_new(HOS_EVENT);
    hos_wait(q->thread, &never, 1, 0, 300000000, NULL);
}

static void get_channel(HosRequest* q)
{
    uint32_t s;
    memcpy(&s, status, 4);
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, s != NOT_CONNECTED ? channel : 0);
}

static void shutdown_uds(HosRequest* q)
{
    initialized = 0;
    for (int i = 0; i < 16; i++) {
        signal_event(binds[i].event);
        binds[i].bind_id = 0;
    }
    ok(q);
}

static const HosCommandEntry commands[] = {
    {0x0003, shutdown_uds, "Shutdown"},
    {0x0008, destroy_network, "DestroyNetwork"},
    {0x000B, get_connection_status, "GetConnectionStatus"},
    {0x000D, get_node_information, "GetNodeInformation"},
    {0x000F, recv_beacon_broadcast_data, "RecvBeaconBroadcastData"},
    {0x0010, set_application_data, "SetApplicationData"},
    {0x0012, bind, "Bind"},
    {0x0014, pull_packet, "PullPacket"},
    {0x001A, get_channel, "GetChannel"},
    {0x001B, initialize_with_version, "InitializeWithVersion"},
    {0x001D, begin_hosting_network, "BeginHostingNetwork"},
    {0x0021, ok, "SetProbeResponseParam"}, /* (Azahar has none: it answers success, ReportUnimplementedFunction) */
    {0x0023, ok, "(unnamed)"},              /* (not in Azahar's table: the same success) */
    {0, NULL, NULL},
};

const HosService hos_nwm_uds = {"nwm::UDS", commands};
