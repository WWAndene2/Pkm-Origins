/* frd:u: friends, as Azahar answers offline (service/frd/frd.cpp): a zero friend key, a zero presence, no friends. */
#include "../Ipc.h"

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void get_my_friend_key(HosRequest* q)
{
    hos_reply(q, 5, 0);
    hos_out(q, 1, HOS_OK);
    for (int k = 2; k <= 5; k++)
        hos_out(q, k, 0);
}

static void get_my_presence(HosRequest* q)
{
    static const uint8_t presence[0x12C];
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_static(q, 2, 0, presence, sizeof presence);
}

static void get_friend_key_list(HosRequest* q)
{
    uint32_t count = hos_in(q, 2);
    static const uint8_t keys[16 * 100];
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 0); /* no friends */
    hos_out_static(q, 3, 0, keys, 16 * count < sizeof keys ? 16 * count : sizeof keys);
}

static const HosCommandEntry commands[] = {
    {0x0005, get_my_friend_key, "GetMyFriendKey"},
    {0x0008, get_my_presence, "GetMyPresence"},
    {0x0011, get_friend_key_list, "GetFriendKeyList"},
    {0x0032, ok, "SetClientSdkVersion"},
    {0, NULL, NULL},
};

const HosService hos_frd_u = {"frd:u", commands};
