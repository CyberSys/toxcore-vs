/* group.c
 *
 * Slightly better groupchats implementation.
 *
 *  Copyright (C) 2014 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "group.h"

#include "util.h"

typedef ptrdiff_t aint;

#define CLOSEST( g, i ) (((g)->closest_peers_entry & (1ull << (i))) != 0)
#define CLOSEST_SET( g, i ) { (g)->closest_peers_entry |= (1ull << (i)); }

typedef enum {
    UNS_NONE,
    UNS_TEMP,
    UNS_FOREVER,

} UnsubscribeType;

/* Interval in seconds to send ping messages */
#define GROUP_PING_INTERVAL 20

enum {
    GROUP_MESSAGE_PING_ID = 0,
    GROUP_MESSAGE_UNSUBSCRIBE_ID = 1,
    GROUP_MESSAGE_NICKNAME_ID = 2,
    GROUP_MESSAGE_NEW_PEER_ID = 16,
    GROUP_MESSAGE_KILL_PEER_ID = 17,
    GROUP_MESSAGE_NAME_ID = 48,
    GROUP_MESSAGE_TITLE_ID = 49
};

#define GROUP_MESSAGE_NEW_PEER_LENGTH (sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE * 2)
#define GROUP_MESSAGE_KILL_PEER_LENGTH (sizeof(uint16_t))

#define MAX_GROUP_MESSAGE_DATA_LEN (MAX_CRYPTO_DATA_SIZE - (1 + MIN_MESSAGE_PACKET_LEN))

enum {
    INVITE_ID = 0,
    INVITE_RESPONSE_ID = 1,
    INVITE_UNSUBSCRIBE_ID = 2,
    INVITE_MYGROUP_ID = 3,
};

#define INVITE_PACKET_SIZE (1 + sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH)
#define INVITE_RESPONSE_PACKET_SIZE (1 + sizeof(uint16_t) * 2 + GROUP_IDENTIFIER_LENGTH)

#define ONLINE_PACKET_DATA_SIZE (sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH)

enum {
    PEER_KILL_ID = 1,
    PEER_QUERY_ID = 8,
    PEER_RESPONSE_ID = 9,
    PEER_TITLE_ID = 10,
    PEER_GROUP_NUM_ID = 11,
};

#define MIN_MESSAGE_PACKET_LEN (sizeof(uint16_t) * 2 + sizeof(uint32_t) + 1)

#define MAX_FAILED_JOIN_ATTEMPTS 16

static void group_name_send(const Group_Chats *g_c, aint groupnumber, const uint8_t *nick, aint nick_len);
static aint send_packet_online(Friend_Connections *fr_c, aint friendcon_id, uint16_t group_num, uint8_t *identifier);
static void send_peer_kill(Group_Chats *g_c, aint friendcon_id, uint16_t group_num);
static int handle_packet(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata);
static int handle_lossy(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata);
static void send_peers(Group_Chats *g_c, aint groupnumber, int friendcon_id, uint16_t other_group_num);
static int get_self_peer_gid(Group_c *g);

static bool really_connected(Group_Peer *peer)
{
    return peer->connected && peer->friendcon_id >= 0 && peer->group_number != 0xffff;
}

static bool is_groupnumber_valid(const Group_Chats *g_c, aint groupnumber)
{
    if ((unsigned)groupnumber >= g_c->num_chats) {
        return false;
    }

    if (!g_c->chats) {
        return false;
    }

    return g_c->chats[groupnumber].live;
}


/* Set the size of the groupchat list to num.
 *
 *  return -1 if realloc fails.
 *  return 0 if it succeeds.
 */
static aint realloc_groupchats(Group_Chats *g_c, aint num)
{
    if (num == 0) {
        free(g_c->chats);
        g_c->chats = NULL;
        return 0;
    }

    Group_c *newgroup_chats = (Group_c *)realloc(g_c->chats, num * sizeof(Group_c));

    if (newgroup_chats == NULL) {
        return -1;
    }

    g_c->chats = newgroup_chats;
    return 0;
}

static void setup_conference(Group_c *g)
{
    memset(g, 0, sizeof(Group_c));
    g->keep_join_index = -1;
    g->live = true;
}

/* Create a new empty groupchat connection.
 *
 * return -1 on failure.
 * return groupnumber on success.
 */
static aint create_group_chat(Group_Chats *g_c)
{
    aint i;

    for (i = 0; i < g_c->num_chats; ++i) {
        if (!g_c->chats[i].live) {
            setup_conference(g_c->chats + i);
            return i;
        }
    }

    aint id = -1;

    if (realloc_groupchats(g_c, g_c->num_chats + 1) == 0) {
        id = g_c->num_chats;
        ++g_c->num_chats;
        setup_conference(g_c->chats + id);
    }

    return id;
}

static Group_c *get_group_c(const Group_Chats *g_c, aint groupnumber)
{
    if (!is_groupnumber_valid(g_c, groupnumber)) {
        return NULL;
    }

    return g_c->chats + groupnumber;
}

/*
 * check if peer with real_pk is in peer array.
 *
 * return peer index if peer is in chat.
 * return -1 if peer is not in chat.
 *
 * TODO(irungentoo): make this more efficient.
 */

static aint peer_in_chat(const Group_c *chat, const uint8_t *real_pk)
{
    size_t i;

    for (i = 0; i < chat->numpeers; ++i) {
        if (chat->peers[i].friendcon_id != -2 && id_equal(chat->peers[i].real_pk, real_pk)) {
            return i;
        }
    }

    return -1;
}

/*
 * check if group with identifier is in group array.
 *
 * return group number if peer is in list.
 * return -1 if group is not in list.
 *
 * TODO(irungentoo): make this more efficient and maybe use constant time comparisons?
 */
static int get_group_num(const Group_Chats *g_c, const uint8_t *identifier)
{
    aint i;

    for (i = 0; i < g_c->num_chats; ++i) {
        if (g_c->chats[i].live && crypto_memcmp(g_c->chats[i].identifier, identifier, GROUP_IDENTIFIER_LENGTH) == 0) {
            return (int)i;
        }
    }

    return -1;
}

int conference_by_uid(const Group_Chats *g_c, const uint8_t *uid)
{
    aint i;

    for (i = 0; i < g_c->num_chats; ++i) {
        if (!g_c->chats[i].live) {
            continue;
        }

        if (crypto_memcmp(g_c->chats[i].identifier + 1, uid, GROUP_IDENTIFIER_LENGTH - 1) == 0) {
            return (int)i;
        }
    }

    return -1;

}

/*
 * check if peer with peer_number is in peer array.
 *
 * return peer number if peer is in chat.
 * return -1 if peer is not in chat.
 *
 */
static aint get_peer_index(const Group_c *g, uint16_t peer_gid)
{
    aint i;

    for (i = 0; i < g->numpeers; ++i) {
        if (g->peers[i].gid == (int)peer_gid) {
            return i;
        }
    }

    return -1;
}

static uint16_t find_new_peer_gid(const Group_c *g)
{
    uint16_t peer_number = random_u16();

    size_t tries = 0;

    while (get_peer_index(g, peer_number) != -1) {
        peer_number = random_u16();
        ++tries;

        if (tries > 32) {
            /* just find first unused peer number */
            peer_number = 0;

            while (get_peer_index(g, peer_number) != -1) {
                ++peer_number;
            }

            break;
        }
    }

    return peer_number;
}

static uint64_t calculate_comp_value(const uint8_t *pk1, const uint8_t *pk2)
{
    uint64_t cmp1 = 0, cmp2 = 0;

    size_t i;

    for (i = 0; i < sizeof(uint64_t); ++i) {
        cmp1 = (cmp1 << 8) + (uint64_t)pk1[i];
        cmp2 = (cmp2 << 8) + (uint64_t)pk2[i];
    }

    return (cmp1 - cmp2);
}

static void add_closest(Group_c *g, size_t peerindex)
{
    size_t i;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {

        if (!CLOSEST(g, i)) {

            g->closest_peers[i] = (uint16_t)peerindex;
            CLOSEST_SET(g, i);
            return;
        }
    }

    size_t index = DESIRED_CLOSE_CONNECTIONS;

    const uint8_t *real_pk = g->peers[peerindex].real_pk;
    uint64_t comp_val = calculate_comp_value(g->real_pk, real_pk);
    uint64_t comp_d = 0;

    for (i = 0; i < (DESIRED_CLOSE_CONNECTIONS / 2); ++i) {

        uint64_t comp;
        comp = calculate_comp_value(g->real_pk, g->peers[g->closest_peers[i]].real_pk);

        if (comp > comp_val && comp > comp_d) {
            index = i;
            comp_d = comp;
        }
    }

    comp_val = calculate_comp_value(real_pk, g->real_pk);

    for (i = (DESIRED_CLOSE_CONNECTIONS / 2); i < DESIRED_CLOSE_CONNECTIONS; ++i) {

        uint64_t comp = calculate_comp_value(g->peers[g->closest_peers[i]].real_pk, g->real_pk);

        if (comp > comp_val && comp > comp_d) {
            index = i;
            comp_d = comp;
        }
    }

    if (index < DESIRED_CLOSE_CONNECTIONS) {
        aint rmpeer = g->closest_peers[index];
        g->closest_peers[index] = (uint16_t)peerindex;
        g->closest_peers_entry |= 1 << index;

        add_closest(g, rmpeer);
    }
}

static int handle_status(void *object, int friendcon_id, uint8_t status, void *userdata)
{
    if (status) {
        return 0;
    }

    size_t ig, i;
    Group_Chats *g_c = (Group_Chats *)object;

    for (ig = 0; ig < g_c->num_chats; ++ig) {

        Group_c *g = get_group_c(g_c, ig);

        if (!g) {
            continue;
        }

        for (i = 0; i < g->numpeers; ++i) {

            if (g->peers[i].friendcon_id == friendcon_id) {
                g->peers[i].friendcon_id = -1;
                g->peers[i].group_number = 0xffff;
                g->peers[i].connected = false;
            }
        }
    }

    return 0;
}

static void apply_changes_in_peers(Group_Chats *g_c, aint groupnumber, void *userdata)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    bool some_changes = false;

    if (g->dirty_list) {

        size_t i, j, empty_slots_in_list = 0;

        /* detect almost deleted peers and prepare peers_list slots for delete */
        for (i = 0; i < g->numpeers_in_list; ++i) {

            aint peer_index = g->peers_list[i];

            if (g->peers[peer_index].friendcon_id == -2 || g->peers[peer_index].gid < 0) {

                some_changes = true;
                g->peers_list[i] = 0xffff;
                ++empty_slots_in_list;
            }
        }

        aint new_size_of_list = 0;

        /* Put new peers to just deleted slots */
        for (i = 0; i < g->numpeers; ++i) {

            const Group_Peer *peer = g->peers + i;

            if (peer->friendcon_id != -2 && peer->gid >= 0) {
                ++new_size_of_list;

                if (empty_slots_in_list > 0) {

                    aint empty_slot = -1;
                    bool present = false;

                    for (j = 0; j < g->numpeers_in_list; ++j) {
                        if (empty_slot < 0 && g->peers_list[j] == 0xffff) {
                            empty_slot = j;
                        }

                        if (g->peers_list[j] == i) {
                            present = true;
                        }

                        if (empty_slot >= 0 && present) {
                            break;
                        }
                    }

                    if (!present) {
                        g->peers_list[empty_slot] = (uint16_t)i;
                        --empty_slots_in_list;
                        some_changes = true;
                    }
                }
            }
        }

        if (empty_slots_in_list > 0) {

            /* all new peers were put into empty slots, so there are no new peers */
            /* just remove empty slots both in peers_list and peers*/

            /* remove empty slots in peers_list */
            for (j = 0; j < g->numpeers_in_list;) {

                if (g->peers_list[j] == 0xffff) {
                    --g->numpeers_in_list;
                    /* g->peers_list[j] = g->peers_list[g->numpeers_in_list]; */ /* faster */
                    memcpy(g->peers_list + j, g->peers_list + j + 1,
                           sizeof(uint16_t) * (g->numpeers_in_list - j)); /* slower, but accurate */
                    some_changes = true;
                    continue;
                }

                ++j;
            }

            /* remove empty slots in peers */
            for (i = 0; i < g->numpeers;) {

                Group_Peer *peer = g->peers + i;

                if (peer->friendcon_id == -2) {
                    --g->numpeers;
                    memcpy(peer, g->peers + g->numpeers, sizeof(Group_Peer));

                    /* fix index in peers_list */
                    for (j = 0; j < g->numpeers_in_list; ++j) {
                        if (g->peers_list[j] == g->numpeers) {
                            g->peers_list[j] = (uint16_t)i;
                            break;
                        }
                    }

                    continue;
                }

                ++i;
            }

        } else if (new_size_of_list > g->numpeers_in_list) {

            /* Expand peers_list and put indexes of new peers into it */

            uint16_t *temp = (uint16_t *)realloc(g->peers_list, sizeof(uint16_t) * new_size_of_list);

            if (temp) {

                g->peers_list = temp;

                for (i = 0; i < g->numpeers; ++i) {

                    const Group_Peer *peer = g->peers + i;

                    if (peer->friendcon_id != -2 && peer->gid >= 0) {

                        bool present = false;

                        for (j = 0; j < g->numpeers_in_list; ++j) {
                            if (g->peers_list[j] == i) {
                                present = true;
                                break;
                            }
                        }

                        if (!present) {

                            g->peers_list[g->numpeers_in_list] = (uint16_t)i;
                            ++g->numpeers_in_list;
                            some_changes = true;
                        }
                    }
                }
            }
        }

        /* Now notify client by calling callbacks */
        if (g_c->group_namelistchange) {
            if (some_changes) {

                if (!g->invite_called && !g->join_mode) {
                    if (g_c->invite_callback) {

                        g->fake_join = true;
                        g_c->invite_callback(g_c->m, UINT32_MAX, g->identifier[0], g->identifier + 1, GROUP_IDENTIFIER_LENGTH - 1, userdata);
                        g->fake_join = false;

                        if (!g->invite_called) {
                            del_groupchat(g_c, (int)groupnumber);
                            return;
                        }
                    }
                }

                g_c->group_namelistchange(g_c->m, (int)groupnumber, 0, CHAT_CHANGE_OCCURRED, userdata);
                g->nick_changed = false;
            }
        }

        /* and now rebuild closest */

        uint16_t old_closest_peers[DESIRED_CLOSE_CONNECTIONS];
        size_t inclose = 0;

        for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
            if (CLOSEST(g,i)) {
                old_closest_peers[inclose++] = g->closest_peers[i];
            }
        }

        g->closest_peers_entry = 0;

        size_t np = g->numpeers;

        for (i = 0; i < np; ++i) {

            Group_Peer *peer = g->peers + i;

            if (peer->friendcon_id == -2 || peer->auto_join || id_equal(g->real_pk, peer->real_pk)) {
                continue;
            }

            add_closest(g, i);
        }

        for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
            if (CLOSEST(g,i)) {
                bool just_added = true;

                for (j = 0; j < inclose; ++j) {
                    if (g->closest_peers[i] == old_closest_peers[j]) {
                        just_added = false;
                        break;
                    }
                }

                Group_Peer *peer = g->peers + g->closest_peers[i];

                if (just_added && peer->connected) {
                    send_packet_online(g_c->fr_c, peer->friendcon_id, (uint16_t)groupnumber, g->identifier);
                }
            }
        }

        g->dirty_list = false;
    }

    /* notify client about nick change */
    if (g->nick_changed && g_c->group_namelistchange) {

        size_t i;

        for (i = 0; i < g->numpeers_in_list; ++i) {
            Group_Peer *peer = g->peers + g->peers_list[i];

            if (peer->nick_changed) {
                g_c->group_namelistchange(g_c->m, (int)groupnumber, (int)i, CHAT_CHANGE_PEER_NAME, userdata);
                peer->nick_changed = false;
            }
        }

        g->nick_changed = false;
    }

    /* notify client about title change */
    if (g->title_changed && g_c->title_callback) {
        size_t i;
        bool cb = false;

        for (i = 0; i < g->numpeers_in_list; ++i) {
            Group_Peer *peer = g->peers + g->peers_list[i];

            if (peer->title_changed) {

                if (!cb) {
                    g_c->title_callback(g_c->m, (int)groupnumber, (int)i, g->title, g->title_len, userdata);
                }

                peer->title_changed = false;
                cb = true;
            }
        }

        if (!cb) {
            g_c->title_callback(g_c->m, (int)groupnumber, -1, g->title, g->title_len, userdata);
        }

        g->title_changed = false;
    }

}

static void send_peer_nums(const Group_Chats *g_c, aint groupnumber, aint friendcon_id, uint16_t other_group_num);

static void connect_to_closest(Group_Chats *g_c, aint groupnumber, void *userdata)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    if (g->dirty_list) {
        return;
    }

    size_t i;

    if (!g->join_mode && is_timeout(g->last_close_check_time, 5)) {
        /* kill connections to non closest */

        g->last_close_check_time = unix_time();
        size_t nconnected = 0;

        for (i = 0; i < g->numpeers; ++i) {
            if (g->peers[i].connected) {
                ++nconnected;
            }
        }

        if (nconnected > DESIRED_CLOSE_CONNECTIONS) {
            for (i = 0; i < g->numpeers; ++i) {

                Group_Peer *peer = g->peers + i;

                if (peer->friendcon_id < 0 || peer->keep_connection) {
                    continue;
                }

                size_t k;

                for (k = 0; k < DESIRED_CLOSE_CONNECTIONS; ++k) {

                    if (CLOSEST(g,k) && g->closest_peers[k] == i) {
                        k = DESIRED_CLOSE_CONNECTIONS + 100;
                        break;
                    }
                }

                if (k == DESIRED_CLOSE_CONNECTIONS + 100) {
                    continue;
                }

                if (really_connected(peer)) {
                    send_peer_kill(g_c, peer->friendcon_id, peer->group_number);
                }

                kill_friend_connection(g_c->fr_c, peer->friendcon_id);
                peer->friendcon_id = -1;
                peer->group_number = 0xffff;
                peer->connected = false;
            }
        }
    }

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
        if (!CLOSEST(g,i)) {
            continue;
        }

        Group_Peer *peer = g->peers + g->closest_peers[i];

        if (peer->friendcon_id < 0) {

            int friendcon_id = getfriend_conn_id_pk(g_c->fr_c, peer->real_pk);

            bool addref = true;

            if (friendcon_id < 0) {

                friendcon_id = new_friend_connection(g_c->fr_c, peer->real_pk);
                addref = false;

                if (friendcon_id == -1) {
                    continue;
                }

                set_dht_temp_pk(g_c->fr_c, friendcon_id, peer->temp_pk, userdata);
            }

            if (addref) {
                friend_connection_lock(g_c->fr_c, friendcon_id);
            }

            peer->friendcon_id = friendcon_id;

            friend_connection_callbacks(g_c->m->fr_c, friendcon_id, GROUPCHAT_CALLBACK_INDEX, &handle_status, &handle_packet,
                                        &handle_lossy, g_c, friendcon_id);

        }

        if (friend_con_connected(g_c->fr_c, peer->friendcon_id) == FRIENDCONN_STATUS_CONNECTED) {

            if (!peer->connected) {
                peer->connected = true;
                peer->need_send_peers = true;
                g->need_send_name = true;
                send_packet_online(g_c->fr_c, peer->friendcon_id, (uint16_t)groupnumber, g->identifier);
            }
        } else {
            peer->connected = false;
        }

        if (peer->need_send_peers && really_connected(peer)) {
            send_peers(g_c, groupnumber, peer->friendcon_id, peer->group_number);
            send_peer_nums(g_c, groupnumber, peer->friendcon_id, peer->group_number);
            peer->need_send_peers = false;
        }
    }


    if (g->need_send_name) {
        group_name_send(g_c, groupnumber, g_c->m->name, g_c->m->name_length);
    }

}

static void addjoinpeer(Group_c *g, const uint8_t *real_pk)
{
    if (id_equal(g->real_pk, real_pk)) {
        return;
    }

    size_t i;
    bool already_here = false;

    for (i = 0; i < g->numjoinpeers; ++i) {
        if (!already_here && id_equal(g->joinpeers[i].real_pk, real_pk)) {
            already_here = true;
            g->joinpeers[i].unsubscribed = false;
        }

        g->joinpeers[i].fails = MAX_FAILED_JOIN_ATTEMPTS;
    }

    if (!already_here) {

        Group_Join_Peer *temp = (Group_Join_Peer *)realloc(g->joinpeers, sizeof(Group_Join_Peer) * (g->numjoinpeers + 1));

        if (temp == NULL) {
            return;
        }

        g->joinpeers = temp;
        temp = g->joinpeers + g->numjoinpeers;
        ++g->numjoinpeers;

        memset(temp, 0, sizeof(Group_Join_Peer));
        id_copy(temp->real_pk, real_pk);
        temp->fails = MAX_FAILED_JOIN_ATTEMPTS;
    }

}

static void need_send_peers(Group_c *g)
{
    size_t i;

    for (i = 0; i < g->numpeers; ++i) {
        g->peers[i].need_send_peers = true;
    }
}

static aint addpeer(Group_c *g, aint groupnumber, const uint8_t *real_pk, const uint8_t *temp_pk, int peer_gid)
{
    if (peer_gid >= 0) {
        addjoinpeer(g, real_pk);
    }

    aint peer_index = peer_in_chat(g, real_pk);

    if (peer_index != -1) {

        id_copy(g->peers[peer_index].temp_pk, temp_pk);

        if (peer_gid != g->peers[peer_index].gid) {

            g->dirty_list = true;

            if (peer_gid >= 0) {
                need_send_peers(g);
            }

            g->peers[peer_index].gid = peer_gid;

            if (peer_gid >= 0) {
                g->peers[peer_index].auto_join = false;
            }
        }

        return peer_index;
    }

    peer_index = get_peer_index(g, peer_gid);

    if (peer_index != -1) {
        return -1;
    }

    Group_Peer *newpeer = (Group_Peer *)realloc(g->peers, sizeof(Group_Peer) * (g->numpeers + 1));

    if (newpeer == NULL) {
        return -1;
    }

    g->peers = newpeer;
    newpeer = newpeer + g->numpeers;
    ++g->numpeers;

    /* Group_Peer constructor */

    memset(newpeer, 0, sizeof(Group_Peer));
    newpeer->friendcon_id = -1; /* set undefined */
    newpeer->group_number = id_equal(g->real_pk, real_pk) ? (uint16_t)groupnumber : 0xffff;   /* set undefined for other */

    id_copy(newpeer->real_pk, real_pk);
    id_copy(newpeer->temp_pk, temp_pk);

    if (peer_gid >= 0) {
        need_send_peers(g);
        newpeer->gid = peer_gid;
    } else {
        newpeer->gid = -1;
    }

    newpeer->last_recv = unix_time();

    g->dirty_list = true;

    if (g->peer_on_join) {
        g->peer_on_join(g->object, (int)groupnumber, g->numpeers - 1);
    }

    return (g->numpeers - 1);
}

static void free_peer_stuff(Group_Peer *peer)
{
    free(peer->lossy);
    peer->lossy = NULL;

    free(peer->nick);
    peer->nick = NULL;

}

/*
 * Delete a peer from the group chat.
 *
 * return 0 if success
 * return -1 if error.
 */
static int delpeer(Group_Chats *g_c, aint groupnumber, aint peer_index)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    Group_Peer *peer = g->peers + peer_index;

    if (peer->friendcon_id >= 0) {
        kill_friend_connection(g_c->fr_c, peer->friendcon_id);
    }

    peer->friendcon_id = -2; /* -2 means almost deleted peer.
                                It just mark "deleted", not really deleted. See apply_changes_in_peers */
    peer->connected = false;
    peer->group_number = 0xffff;
    peer->gid = -1;
    free_peer_stuff(peer);

    g->dirty_list = true;

    if (g->peer_on_leave) {
        g->peer_on_leave(g->object, (int)groupnumber, peer->object);
    }

    peer->object = NULL;

    return 0;
}

int find_peer_index_in_list(const Group_c *g, aint peer_index)
{
    size_t i;

    for (i = 0; i < g->numpeers_in_list; ++i) {
        if (g->peers_list[i] == peer_index) {
            return (int)i;
        }
    }

    return -1;
}

/* Set the nick for a peer.
 *
 * return 0 on success.
 * return -1 if error.
 */
static aint setnick(Group_c *g, aint peer_index, const uint8_t *nick, aint nick_len)
{
    if (nick_len > MAX_NAME_LENGTH || nick_len == 0) {
        return -1;
    }

    Group_Peer *peer = g->peers + peer_index;

    /* same name as already stored? */
    if (peer->nick_len == nick_len) {
        if (nick_len == 0 || !memcmp(peer->nick, nick, nick_len)) {
            return 0;
        }
    }

    if (nick_len) {

        if (nick_len > peer->nick_len) {
            uint8_t *new_nick = realloc(peer->nick, nick_len + 1);

            if (!new_nick) {
                return 0;
            }

            peer->nick = new_nick;
            peer->nick[nick_len] = 0;
        }

        memcpy(peer->nick, nick, nick_len);
    }

    peer->nick_len = (uint8_t)nick_len;
    peer->nick_changed = true;
    g->nick_changed = true;
    return 0;
}

static aint settitle(Group_c *g, aint peer_index, const uint8_t *title, aint title_len)
{
    if (title_len > MAX_NAME_LENGTH || title_len == 0) {
        return -1;
    }

    /* same as already set? */
    if (g->title_len == title_len && !memcmp(g->title, title, title_len)) {
        return 0;
    }

    memcpy(g->title, title, title_len);
    g->title_len = (uint8_t)title_len;
    g->title_changed = true;

    if (peer_index >= 0) {
        g->peers[peer_index].title_changed = true;
    }

    return 0;
}

/* Creates a new groupchat and puts it in the chats array.
 *
 * type is one of GROUPCHAT_TYPE_*
 *
 * return group number on success.
 * return -1 on failure.
 */
int add_groupchat(Group_Chats *g_c, uint8_t type, const uint8_t *uid)
{
    aint groupnumber = create_group_chat(g_c);

    if (groupnumber == -1) {
        return -1;
    }

    Group_c *g = g_c->chats + groupnumber;

    g->invite_called = true;

    if (uid != NULL) {
        memcpy(g->identifier + 1, uid, GROUP_IDENTIFIER_LENGTH - 1);
    } else {
        new_symmetric_key(g->identifier + 1);
    }

    g->identifier[0] = type;
    memcpy(g->real_pk, nc_get_self_public_key(g_c->m->net_crypto), CRYPTO_PUBLIC_KEY_SIZE);
    aint peer_index = addpeer(g, groupnumber, g->real_pk, dht_get_self_public_key(g_c->m->dht), -1);

    if (peer_index == -1) {
        return -1;
    }

    g->peers[peer_index].group_number = (uint16_t)groupnumber;

    setnick(g, peer_index, g_c->m->name, g_c->m->name_length);

    return (int)groupnumber;
}

static Group_c *group_kill_peer_send(const Group_Chats *g_c, aint groupnumber);

static void on_offline(Group_c *g)
{
    g->join_mode = true;
    g->fake_join = false;
    g->auto_join = false;
    g->need_send_name = false;
    g->title_changed = false;
    g->invite_called = false;
    g->keep_leave = false;
    g->disable_auto_join = false;
    g->keep_join_index = -1;

    size_t i;

    for (i = 0; i < g->numjoinpeers; ++i) {

        Group_Join_Peer *j = g->joinpeers + i;
        j->unsubscribed = false;
        j->online = false;
        j->fails = 0;
    }
}

int enter_conference(Group_Chats *g_c, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (!g->disable_auto_join) {
        return -2;
    }

    on_offline(g);
    return 0;
}

static int conference_unsubscribe(const Group_Chats *g_c, aint groupnumber, UnsubscribeType u);

static Group_c *disconnect_conference(const Group_Chats *g_c, aint groupnumber, UnsubscribeType u)
{
    if (u) {
        conference_unsubscribe(g_c, groupnumber, u);
    }

    Group_c *g = group_kill_peer_send(g_c, groupnumber);

    if (!g) {
        return NULL;
    }

    size_t i;

    memset(g->closest_peers, 0, sizeof(g->closest_peers));
    g->closest_peers_entry = 0;

    g->invite_called = false;

    g->message_number = 0;
    g->lossy_message_number = 0;
    g->keep_join_index = -1;

    for (i = 0; i < g->numpeers; ++i) {

        Group_Peer *peer = g->peers + i;

        if (peer->friendcon_id == -2) {
            continue;
        }

        if (peer->friendcon_id >= 0) {
            kill_friend_connection(g_c->fr_c, peer->friendcon_id);
        }

        if (g->peer_on_leave) {
            g->peer_on_leave(g->object, (int)groupnumber, peer->object);
        }

        free_peer_stuff(peer);
    }

    free(g->peers);
    g->numpeers = 0;
    g->peers = NULL;
    free(g->peers_list);
    g->numpeers_in_list = 0;
    g->peers_list = NULL;

    if (g->group_on_delete) {
        g->group_on_delete(g->object, (int)groupnumber);
    }

    g->object = NULL;
    g->peer_on_leave = NULL;
    g->peer_on_join = NULL;
    g->group_on_delete = NULL;

    return g;
}

int leave_conference(Group_Chats *g_c, int groupnumber, bool keep_leave)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->keep_leave = keep_leave;

    if (g->disable_auto_join) {
        return -2;
    }

    disconnect_conference(g_c, groupnumber, UNS_TEMP);

    g->dirty_list = true;
    g->disable_auto_join = true;
    g->keep_leave = keep_leave;
    g->join_mode = false;
    g->fake_join = false;
    g->auto_join = false;
    g->need_send_name = false;
    g->title_changed = false;
    g->invite_called = false;

    return 0;
}

/* Delete a groupchat from the chats array.
 *
 * return 0 on success.
 * return -1 if groupnumber is invalid.
 */
static int del_groupchat_internal(Group_Chats *g_c, aint groupnumber, UnsubscribeType uns)
{
    Group_c *g = disconnect_conference(g_c, groupnumber, uns);

    if (!g) {
        return -1;
    }

    free(g->joinpeers);

    crypto_memzero(g_c->chats + groupnumber, sizeof(Group_c));

    size_t i;

    for (i = groupnumber + 1; i < g_c->num_chats; ++i) {
        if (g_c->chats[i].live) {
            return 0;
        }
    }

    g_c->num_chats = (uint16_t)groupnumber;
    realloc_groupchats(g_c, groupnumber);

    return 0;
}

int del_groupchat(Group_Chats *g_c, int groupnumber)
{
    return del_groupchat_internal(g_c, groupnumber, UNS_FOREVER);
}

/* Copy the public key of peernumber who is in groupnumber to pk.
 * pk must be CRYPTO_PUBLIC_KEY_SIZE long.
 *
 * return 0 on success
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 */
int group_peer_pubkey(const Group_Chats *g_c, int groupnumber, int peernumber, uint8_t *pk)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (peernumber < 0 || (uint32_t)peernumber >= g->numpeers_in_list) {
        return -2;
    }

    if (g->peers_list[peernumber] == 0xffff) {
        return -2;
    }

    memcpy(pk, g->peers[g->peers_list[peernumber]].real_pk, CRYPTO_PUBLIC_KEY_SIZE);
    return 0;
}

/*
 * Return the size of peernumber's name.
 *
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 */
int group_peername_size(const Group_Chats *g_c, int groupnumber, int peernumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (peernumber < 0 || (uint32_t)peernumber >= g->numpeers_in_list) {
        return -2;
    }

    if (g->peers_list[peernumber] == 0xffff) {
        return -2;
    }

    const Group_Peer *peer = g->peers + g->peers_list[peernumber];

    if (peer->nick_len == 0) {
        return 8;
    }

    return peer->nick_len;
}

/* Copy the name of peernumber who is in groupnumber to name.
 * name must be at least MAX_NAME_LENGTH long.
 *
 * return length of name if success
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 */
int group_peername(const Group_Chats *g_c, int groupnumber, int peernumber, uint8_t *name)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (peernumber < 0 || (uint32_t)peernumber >= g->numpeers_in_list) {
        return -2;
    }

    if (g->peers_list[peernumber] == 0xffff) {
        return -2;
    }

    Group_Peer *peer = g->peers + g->peers_list[peernumber];

    peer->nick_changed = false;

    if (peer->nick_len == 0) {
        memcpy(name, "Tox User", 8);
        return 8;
    }

    memcpy(name, peer->nick, peer->nick_len);
    return peer->nick_len;
}

/* List all the peers in the group chat.
 *
 * Copies the names of the peers to the name[length][MAX_NAME_LENGTH] array.
 *
 * Copies the lengths of the names to lengths[length]
 *
 * returns the number of peers on success.
 *
 * return -1 on failure.
 */
int group_names(const Group_Chats *g_c, int groupnumber, uint8_t names[][MAX_NAME_LENGTH], uint16_t lengths[],
                uint16_t length)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    unsigned i;

    for (i = 0; i < g->numpeers_in_list && i < length; ++i) {
        int l = group_peername(g_c, groupnumber, i, names[i]);

        if (l >= 0) {
            lengths[i] = l;
        } else {
            lengths[i] = 0;
            names[i][0] = 0;
        }
    }

    return i;
}

/* Return the number of peers in the group chat on success.
 * return -1 if groupnumber is invalid.
 */
int group_number_peers(const Group_Chats *g_c, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    return g->numpeers_in_list;
}

/* return 1 if the peernumber corresponds to ours.
 * return 0 if the peernumber is not ours.
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 * return -3 if we are not connected to the group chat.
 */
int group_peernumber_is_ours(const Group_Chats *g_c, int groupnumber, int peer_index)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((unsigned)peer_index >= g->numpeers_in_list) {
        return -2;
    }

    if (!g->live) {
        return -3;
    }

    const Group_Peer *peer = g->peers + g->peers_list[peer_index];
    return id_equal(g->real_pk, peer->real_pk);
}

/* return the type of groupchat (GROUPCHAT_TYPE_) that groupnumber is.
 *
 * return -1 on failure.
 * return type on success.
 */
int group_get_type(const Group_Chats *g_c, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    return g->identifier[0];
}

/* return the unique id of group chat that groupnumber is.
*
* return -1 on failure.
* return type on success.
*/
int conference_get_id(const Group_Chats *g_c, int groupnumber, uint8_t *uid)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    memcpy(uid, g->identifier + 1, sizeof(g->identifier) - 1);
    return 0;
}

/* Send a group packet to friendcon_id.
 *
 *  return 1 on success
 *  return 0 on failure
 */
static bool send_packet_group_peer(Friend_Connections *fr_c, aint friendcon_id, uint8_t packet_id,
                                   uint16_t other_group_num, const uint8_t *data, aint length)
{
    size_t plen = 1 + sizeof(uint16_t) + length;

    if (plen > MAX_CRYPTO_DATA_SIZE) {
        return false;
    }

    other_group_num = htons(other_group_num);
    uint8_t  packet[MAX_CRYPTO_DATA_SIZE];
    packet[0] = packet_id;
    memcpy(packet + 1, &other_group_num, sizeof(uint16_t));
    memcpy(packet + 1 + sizeof(uint16_t), data, length);
    return write_cryptpacket(friendconn_net_crypto(fr_c), friend_connection_crypt_connection_id(fr_c, (int)friendcon_id), packet,
                             (uint16_t)plen, 0) != -1;
}

/* Send a group lossy packet to friendcon_id.
 *
 *  return 1 on success
 *  return 0 on failure
 */
static bool send_lossy_group_peer(Friend_Connections *fr_c, aint friendcon_id, uint8_t packet_id,
                                  uint16_t group_num, const uint8_t *data, aint length)
{
    size_t plen = 1 + sizeof(uint16_t) + length;

    if (plen > MAX_CRYPTO_DATA_SIZE) {
        return false;
    }

    group_num = htons(group_num);
    uint8_t packet[MAX_CRYPTO_DATA_SIZE];
    packet[0] = packet_id;
    memcpy(packet + 1, &group_num, sizeof(uint16_t));
    memcpy(packet + 1 + sizeof(uint16_t), data, length);
    return send_lossy_cryptpacket(friendconn_net_crypto(fr_c), friend_connection_crypt_connection_id(fr_c, (int)friendcon_id), packet,
                                  (uint16_t)plen) != -1;
}

/* invite friendnumber to groupnumber.
 *
 * return 0 on success.
 * return -1 if groupnumber is invalid.
 * return -2 if invite packet failed to send.
 */
int invite_friend(Group_Chats *g_c, int32_t friendnumber, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    uint8_t invite[INVITE_PACKET_SIZE];
    invite[0] = INVITE_ID;
    uint16_t groupchat_num = htons((uint16_t)groupnumber);
    memcpy(invite + 1, &groupchat_num, sizeof(groupchat_num));
    memcpy(invite + 1 + sizeof(groupchat_num), g->identifier, GROUP_IDENTIFIER_LENGTH);

    if (send_conference_invite_packet(g_c->m, friendnumber, invite, sizeof(invite))) {
        return 0;
    }

    return -2;
}

static aint send_message_group(const Group_Chats *g_c, aint groupnumber, uint8_t message_id, const uint8_t *data,
                               aint len);

static void send_peer_query(const Group_Chats *g_c, aint friendcon_id, uint16_t other_group_num);

/* Join a group (you need to have been invited first.)
 *
 * expected_type is the groupchat type we expect the chat we are joining is.
 *
 * return group number on success.
 * return -1 if data length is invalid.
 * return -2 if group is not the expected type.
 * return -3 if friendnumber is invalid.
 * return -4 if client is already in this group.
 * return -5 if group instance failed to initialize.
 * return -6 if join packet fails to send.
 */
int join_groupchat(Group_Chats *g_c, int32_t friendnumber, uint8_t expected_type, const uint8_t *data, uint16_t length)
{
    if (length == GROUP_IDENTIFIER_LENGTH - 1) {

        int groupnumber = conference_by_uid(g_c, data);

        if (groupnumber != -1) {
            Group_c *g = g_c->chats + groupnumber;
            g->invite_called |= g->fake_join;
            return groupnumber;
        }

        /* create groupchat with exist id */
        return add_groupchat(g_c, expected_type, data);
    }

    if (length != sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH) {
        return -1;
    }

    if (data[sizeof(uint16_t)] != expected_type) {
        return -2;
    }

    bool just_created = false;
    aint groupnumber = get_group_num(g_c, data + sizeof(uint16_t));

    if (groupnumber == -1) {
        groupnumber = create_group_chat(g_c);
        just_created = true;
    }

    if (groupnumber == -1) {
        return -5;
    }

    Group_c *g = g_c->chats + groupnumber;

    if (g->fake_join) {
        g->need_send_name = true;
        g->invite_called = true;
        return (int)groupnumber;
    }

    int friendcon_id = getfriendcon_id(g_c->m, friendnumber);

    if (friendcon_id == -1) {
        return -3;
    }

    memcpy(g->real_pk, nc_get_self_public_key(g_c->m->net_crypto), CRYPTO_PUBLIC_KEY_SIZE);

    g->dirty_list = true;
    g->need_send_name = true;

    aint peer_index = addpeer(g, groupnumber, g->real_pk, dht_get_self_public_key(g_c->m->dht),
                              g->auto_join ? get_self_peer_gid(g) : -1);

    if (peer_index != -1) {
        setnick(g, peer_index, g_c->m->name, g_c->m->name_length);
        g->peers[peer_index].group_number = (uint16_t)groupnumber;
    }

    uint8_t response[INVITE_RESPONSE_PACKET_SIZE];
    response[0] = INVITE_RESPONSE_ID;
    uint16_t group_num = htons((uint16_t)groupnumber);
    memcpy(response + 1, &group_num, sizeof(uint16_t));
    memcpy(response + 1 + sizeof(uint16_t), data, sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH);

    if (send_conference_invite_packet(g_c->m, friendnumber, response, sizeof(response))) {

        if (just_created) {
            memcpy(g->identifier, data + sizeof(uint16_t), GROUP_IDENTIFIER_LENGTH);
            g->invite_called = true; /* just created means client called join_groupchat */
        }

        uint16_t other_groupnum = 0;
        memcpy(&other_groupnum, data, sizeof(other_groupnum));
        other_groupnum = ntohs(other_groupnum);

        uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE], temp_pk[CRYPTO_PUBLIC_KEY_SIZE];
        get_friendcon_public_keys(real_pk, temp_pk, g_c->fr_c, friendcon_id);
        peer_index = addpeer(g, groupnumber, real_pk, temp_pk, -1);

        if (g->peers[peer_index].friendcon_id != friendcon_id) {
            if (g->peers[peer_index].friendcon_id >= 0) {
                kill_friend_connection(g_c->fr_c, g->peers[peer_index].friendcon_id);
                g->peers[peer_index].friendcon_id = -1;
                g->peers[peer_index].connected = false;
            }
        }

        if (g->auto_join) {

            g->peers[peer_index].auto_join = true;
            friend_connection_callbacks(g_c->m->fr_c, friendcon_id, GROUPCHAT_CALLBACK_INDEX, &handle_status, &handle_packet,
                                        &handle_lossy, g_c, friendcon_id);

            if (g->peers[peer_index].friendcon_id < 0) {
                g->peers[peer_index].friendcon_id = friendcon_id;
                g->peers[peer_index].connected = true;
                friend_connection_lock(g_c->fr_c, friendcon_id);
            }

            g->peers[peer_index].group_number = 0xffff;
            g->peers[peer_index].keep_connection = 2;
        } else {
            g->peers[peer_index].group_number = other_groupnum;
        }

        send_peer_query(g_c, friendcon_id, other_groupnum);
        return (int)groupnumber;
    }

    if (just_created) {
        g->live = false;
    }

    return -6;
}

/* Set handlers for custom lossy packets.
 *
 * NOTE: Handler must return 0 if packet is to be relayed, -1 if the packet should not be relayed.
 *
 * Function(void *group object (set with group_set_object), int groupnumber, int friendgroupnumber, void *group peer object (set with group_peer_set_object), const uint8_t *packet, uint16_t length)
 */
void group_lossy_packet_registerhandler(Group_Chats *g_c, uint8_t byte, int (*function)(void *, int, int, void *,
                                        const uint8_t *, uint16_t))
{
    /* g_c->lossy_packethandlers[byte].function = function; */

    if (192 /* GROUP_AUDIO_PACKET_ID */ == byte) {
        g_c->lossy_packethandler = function;
    }


}

/* Set the callback for group invites.
 *
 *  Function(Group_Chats *g_c, int32_t friendnumber, uint8_t type, uint8_t *data, uint16_t length, void *userdata)
 *
 *  data of length is what needs to be passed to join_groupchat().
 */
void g_callback_group_invite(Group_Chats *g_c, void (*function)(Messenger *m, uint32_t, int, const uint8_t *,
                             size_t, void *))
{
    g_c->invite_callback = function;
}

/* Set the callback for group messages.
 *
 *  Function(Group_Chats *g_c, int groupnumber, int friendgroupnumber, uint8_t * message, uint16_t length, void *userdata)
 */
void g_callback_group_message(Group_Chats *g_c, void (*function)(Messenger *m, uint32_t, uint32_t, int, const uint8_t *,
                              size_t, void *))
{
    g_c->message_callback = function;
}

/* Set callback function for peer name list changes.
 *
 * It gets called every time the name list changes(new peer/name, deleted peer)
 *  Function(Group_Chats *g_c, int groupnumber, int peernumber, TOX_CHAT_CHANGE change, void *userdata)
 */
void g_callback_group_namelistchange(Group_Chats *g_c, void (*function)(Messenger *m, int, int, uint8_t, void *))
{
    g_c->group_namelistchange = function;
}

/* Set callback function for title changes.
 *
 * Function(Group_Chats *g_c, int groupnumber, int friendgroupnumber, uint8_t * title, uint8_t length, void *userdata)
 * if friendgroupnumber == -1, then author is unknown (e.g. initial joining the group)
 */
void g_callback_group_title(Group_Chats *g_c, void (*function)(Messenger *m, uint32_t, uint32_t, const uint8_t *,
                            size_t, void *))
{
    g_c->title_callback = function;
}

/* Set a function to be called when a new peer joins a group chat.
 *
 * Function(void *group object (set with group_set_object), int groupnumber, int friendgroupnumber)
 *
 * return 0 on success.
 * return -1 on failure.
 */
int callback_groupchat_peer_new(const Group_Chats *g_c, int groupnumber, void (*function)(void *, int, int))
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (g->peer_on_join != function) {
        g->peer_on_join = function;

        size_t i;

        for (i = 0; i < g->numpeers; ++i) {
            g->peer_on_join(g->object, groupnumber, (int)i);
        }
    }

    return 0;
}

/* Set a function to be called when a peer leaves a group chat.
 *
 * Function(void *group object (set with group_set_object), int groupnumber, int friendgroupnumber, void *group peer object (set with group_peer_set_object))
 *
 * return 0 on success.
 * return -1 on failure.
 */
int callback_groupchat_peer_delete(Group_Chats *g_c, int groupnumber, void (*function)(void *, int, void *))
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->peer_on_leave = function;
    return 0;
}

/* Set a function to be called when the group chat is deleted.
 *
 * Function(void *group object (set with group_set_object), int groupnumber)
 *
 * return 0 on success.
 * return -1 on failure.
 */
int callback_groupchat_delete(Group_Chats *g_c, int groupnumber, void (*function)(void *, int))
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->group_on_delete = function;
    return 0;
}

static int group_ping_send(const Group_Chats *g_c, int groupnumber)
{
    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_PING_ID, 0, 0) > 0) {
        return 0;
    }

    return -1;
}

static int nick_request_send(const Group_Chats *g_c, int groupnumber, int gid)
{
    uint8_t d[sizeof(uint16_t)];
    *(uint16_t *)d = htons((uint16_t)gid);

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_NICKNAME_ID, d, sizeof(d)) > 0) {
        return 0;
    }

    return -1;
}


static Group_Peer *get_self_peer(Group_c *g)
{
    size_t i;

    for (i = 0; i < g->numpeers; ++i) {
        if (id_equal(g->real_pk, g->peers[i].real_pk)) {
            return g->peers + i;
        }
    }

    return NULL;
}


static int get_self_peer_gid(Group_c *g)
{
    size_t i;

    for (i = 0; i < g->numpeers; ++i) {
        if (id_equal(g->real_pk, g->peers[i].real_pk)) {
            return g->peers[i].gid;
        }
    }

    return -1;
}

static void change_self_peer_gid(Group_Chats *g_c, aint groupnumber, int self_peer_gid)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    size_t i;

    for (i = 0; i < g->numpeers; ++i) {
        if (id_equal(g->peers[i].real_pk, g->real_pk)) {

            if (g->peers[i].gid != self_peer_gid) {
                g->dirty_list = true;
                g->peers[i].gid = self_peer_gid;
            }

            return;
        }
    }

    /* no self peer found! Add it */

    aint peer_index = addpeer(g, groupnumber, g->real_pk, dht_get_self_public_key(g_c->m->dht), self_peer_gid);

    if (peer_index == -1) {
        return;
    }

    setnick(g, peer_index, g_c->m->name, g_c->m->name_length);
}

/* send a new_peer message
 * return 0 on success
 * return -1 on failure
 */
static int group_new_peer_send(Group_Chats *g_c, aint groupnumber, uint16_t peer_gid, const uint8_t *real_pk,
                               const uint8_t *temp_pk)
{

    uint8_t packet[GROUP_MESSAGE_NEW_PEER_LENGTH];
    peer_gid = htons(peer_gid);
    memcpy(packet, &peer_gid, sizeof(uint16_t));
    memcpy(packet + sizeof(uint16_t), real_pk, CRYPTO_PUBLIC_KEY_SIZE);
    memcpy(packet + sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE, temp_pk, CRYPTO_PUBLIC_KEY_SIZE);

    /* make self gid valid due self is inviter */
    Group_c *g = g_c->chats + groupnumber;

    if (get_self_peer_gid(g) < 0) {
        change_self_peer_gid(g_c, groupnumber, find_new_peer_gid(g));
    }

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_NEW_PEER_ID, packet, GROUP_MESSAGE_NEW_PEER_LENGTH) > 0) {
        return 0;
    }

    return -1;
}

static int conference_unsubscribe(const Group_Chats *g_c, aint groupnumber, UnsubscribeType u)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    uint8_t packet[GROUP_IDENTIFIER_LENGTH + CRYPTO_PUBLIC_KEY_SIZE];
    packet[0] = (uint8_t)u;
    memcpy(packet + 1, g->identifier + 1, GROUP_IDENTIFIER_LENGTH - 1);
    memcpy(packet + GROUP_IDENTIFIER_LENGTH, g->real_pk, CRYPTO_PUBLIC_KEY_SIZE);

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_UNSUBSCRIBE_ID, packet, sizeof(packet)) > 0) {
        return 0;
    }

    return -1;
}


static void send_peer_nums(const Group_Chats *g_c, aint groupnumber, aint friendcon_id, uint16_t other_group_num)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    uint8_t packet[MAX_CRYPTO_DATA_SIZE - (1 + sizeof(uint16_t))];
    packet[0] = PEER_GROUP_NUM_ID;
    size_t ptr = 1, i;

    for (i = 0; i < g->numpeers; ++i) {

        Group_Peer *peer = g->peers + i;

        if (peer->group_number == 0xffff) {
            continue;
        }

        if (ptr + CRYPTO_PUBLIC_KEY_SIZE + sizeof(uint16_t) > sizeof(packet)) {
            send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, packet, ptr);
            ptr = 1;
        }

        memcpy(packet + ptr, peer->real_pk, CRYPTO_PUBLIC_KEY_SIZE);
        ptr += CRYPTO_PUBLIC_KEY_SIZE;
        *(uint16_t *)(packet + ptr) = htons(peer->group_number);
        ptr += sizeof(uint16_t);
    }

    if (ptr > 1) {
        send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, packet, ptr);
    }
}

static Group_c *group_kill_peer_send(const Group_Chats *g_c, aint groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (g) {

        uint8_t packet[GROUP_MESSAGE_KILL_PEER_LENGTH];

        int self_gid = get_self_peer_gid(g);

        if (self_gid < 0) {
            return g;
        }

        uint16_t peer_gid = htons((uint16_t)self_gid);
        memcpy(packet, &peer_gid, sizeof(uint16_t));
        send_message_group(g_c, groupnumber, GROUP_MESSAGE_KILL_PEER_ID, packet, sizeof(packet));
    }

    return g;
}

static void group_name_send(const Group_Chats *g_c, aint groupnumber, const uint8_t *nick, aint nick_len)
{
    if (nick_len > MAX_NAME_LENGTH) {
        return;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    g->need_send_name = false;
    send_message_group(g_c, groupnumber, GROUP_MESSAGE_NAME_ID, nick, nick_len);
}

/* set the group's title, limited to MAX_NAME_LENGTH
 * return 0 on success
 * return -1 if groupnumber is invalid.
 * return -2 if title is too long or empty.
 * return -3 if packet fails to send.
 */
int group_title_send(const Group_Chats *g_c, int groupnumber, const uint8_t *title, uint8_t title_len)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (title_len > MAX_NAME_LENGTH || title_len == 0) {
        return -2;
    }

    /* same as already set? */
    if (g->title_len == title_len && !memcmp(g->title, title, title_len)) {
        return 0;
    }

    memcpy(g->title, title, title_len);
    g->title_len = title_len;

    if (g->numpeers == 1) {
        return 0;
    }

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_TITLE_ID, title, title_len) > 0) {
        return 0;
    }

    return -3;
}

/* return the group's title size.
 * return -1 of groupnumber is invalid.
 * return -2 if title is too long or empty.
 */
int group_title_get_size(const Group_Chats *g_c, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (g->title_len == 0 || g->title_len > MAX_NAME_LENGTH) {
        return -2;
    }

    return g->title_len;
}

/* Get group title from groupnumber and put it in title.
 * Title needs to be a valid memory location with a size of at least MAX_NAME_LENGTH (128) bytes.
 *
 * return length of copied title if success.
 * return -1 if groupnumber is invalid.
 * return -2 if title is too long or empty.
 */
int group_title_get(const Group_Chats *g_c, int groupnumber, uint8_t *title)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (g->title_len == 0 || g->title_len > MAX_NAME_LENGTH) {
        return -2;
    }

    memcpy(title, g->title, g->title_len);
    return g->title_len;
}

static void unsubscribe_peer(Group_Chats *g_c, const uint8_t *conf_id, const uint8_t *peer_pk, UnsubscribeType u)
{
    Group_c *g = get_group_c(g_c, conference_by_uid(g_c, conf_id));

    if (g != NULL) {

        size_t i;

        for (i = 0; i < g->numjoinpeers; ++i) {
            Group_Join_Peer *jp = g->joinpeers + i;

            if (id_equal(jp->real_pk, peer_pk)) {

                if (UNS_FOREVER == u) {
                    --g->numjoinpeers;

                    if (g->numjoinpeers > 0) {
                        memcpy(jp, g->joinpeers + g->numjoinpeers, sizeof(Group_Join_Peer));

                    } else {
                        free(g->joinpeers);
                        g->joinpeers = NULL;
                    }
                } else if (UNS_TEMP == u) {
                    jp->unsubscribed = true;
                }

                break;
            }
        }
    }
}

static void set_peer_groupnum(Group_c *g, const uint8_t *peer_pk, uint16_t gn)
{
    if (id_equal(g->real_pk, peer_pk)) {
        /* ignore */
        return;
    }

    aint i = peer_in_chat(g, peer_pk);

    if (i >= 0) {

        Group_Peer *peer = g->peers + i;

        if (peer->connected && peer->group_number == 0xffff) {
            peer->group_number = gn;
        }

    }
}


static void handle_friend_invite_packet(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length,
                                        void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)m->conferences_object;

    if (length <= 1) {
        return;
    }

    const uint8_t *invite_data = data + 1;
    uint16_t invite_length = length - 1;

    switch (data[0]) {
        case INVITE_MYGROUP_ID:
        case INVITE_ID: {

            if (length < INVITE_PACKET_SIZE) {
                return;
            }

            aint groupnumber = get_group_num(g_c, data + 1 + sizeof(uint16_t));

            if (groupnumber == -1) {

                if (data[0] == INVITE_MYGROUP_ID) {
                    return;
                }

                if (g_c->invite_callback) {
                    g_c->invite_callback(m, friendnumber, *(invite_data + sizeof(uint16_t)), invite_data, invite_length, userdata);
                }

            } else {

                Group_c *g = get_group_c(g_c, groupnumber);

                if (g->keep_leave) {
                    return;
                }

                g->join_mode = false;
                g->disable_auto_join = false;
                g->invite_called = false;
                join_groupchat(g_c, friendnumber, *(invite_data + sizeof(uint16_t)), invite_data, invite_length);
            }

            return;
        }

        case INVITE_RESPONSE_ID: {

            if (length != INVITE_RESPONSE_PACKET_SIZE) {
                return;
            }

            uint16_t groupnum_in;

            memcpy(&groupnum_in, data + 1 + sizeof(uint16_t), sizeof(uint16_t));
            groupnum_in = ntohs(groupnum_in);

            const uint8_t *conference_id = data + 2 + sizeof(uint16_t) * 2;

            /* we absolutely do not care about groupnumber in packet
             * but, in case groupnum_in != groupnumber,
             * we should send INVITE_ID packet back to peer with correct groupnumber */

            int groupnumber = get_group_num(g_c, data + 1 + sizeof(uint16_t) * 2);
            Group_c *g = get_group_c(g_c, groupnumber);

            if (!g) {

                uint8_t nosuchgroup[GROUP_IDENTIFIER_LENGTH + CRYPTO_PUBLIC_KEY_SIZE];
                nosuchgroup[0] = INVITE_UNSUBSCRIBE_ID;
                memcpy(nosuchgroup + 1, conference_id, GROUP_IDENTIFIER_LENGTH - 1);
                memcpy(nosuchgroup + GROUP_IDENTIFIER_LENGTH, nc_get_self_public_key(g_c->m->net_crypto), CRYPTO_PUBLIC_KEY_SIZE);
                send_conference_invite_packet(g_c->m, friendnumber, nosuchgroup, sizeof(nosuchgroup));
                return;
            }

            if (g->numpeers >= 65534 || g->keep_leave) {
                return;
            }

            if (groupnumber != groupnum_in) {

                /* send INVITE_ID to restore valid groupnum
                 * old toxcore will ignore this packet,
                 */

                uint8_t invite[INVITE_PACKET_SIZE];
                invite[0] = INVITE_MYGROUP_ID;
                uint16_t groupchat_num = htons((uint16_t)groupnumber);
                memcpy(invite + 1, &groupchat_num, sizeof(groupchat_num));
                memcpy(invite + 1 + sizeof(groupchat_num), g->identifier, GROUP_IDENTIFIER_LENGTH);

                send_conference_invite_packet(g_c->m, friendnumber, invite, sizeof(invite));
                return;
            }


            int friendcon_id = getfriendcon_id(m, friendnumber);
            uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE], temp_pk[CRYPTO_PUBLIC_KEY_SIZE];
            get_friendcon_public_keys(real_pk, temp_pk, g_c->fr_c, friendcon_id);

            /* peer_gid collision will be resolved */
            uint16_t peer_gid = find_new_peer_gid(g);

            aint peer_index = addpeer(g, groupnumber, real_pk, temp_pk, peer_gid);

            if (g->peers[peer_index].friendcon_id != friendcon_id) {
                if (g->peers[peer_index].friendcon_id >= 0) {
                    kill_friend_connection(g_c->fr_c, g->peers[peer_index].friendcon_id);
                    g->peers[peer_index].friendcon_id = -1;
                }

                g->peers[peer_index].friendcon_id = friendcon_id;
                g->peers[peer_index].connected = true;
                friend_connection_lock(g_c->fr_c, friendcon_id);
            }

            friend_connection_callbacks(g_c->m->fr_c, friendcon_id, GROUPCHAT_CALLBACK_INDEX, &handle_status, &handle_packet,
                                        &handle_lossy, g_c, friendcon_id);


            g->peers[peer_index].group_number = ntohs(*(uint16_t *)(data + 1));

            g->need_send_name = true;
            group_new_peer_send(g_c, groupnumber, peer_gid, real_pk, temp_pk);

            g->join_mode = false;
            g->keep_leave = false;
            g->disable_auto_join = false;
            break;
        }

        case INVITE_UNSUBSCRIBE_ID: {

            if (length < GROUP_IDENTIFIER_LENGTH + CRYPTO_PUBLIC_KEY_SIZE) {
                return;
            }

            unsubscribe_peer(g_c, data + 1, data + GROUP_IDENTIFIER_LENGTH, UNS_FOREVER);
            break;
        }

        default:
            return;
    }
}

static aint send_packet_online(Friend_Connections *fr_c, aint friendcon_id, uint16_t my_group_num, uint8_t *identifier)
{
    uint8_t packet[1 + ONLINE_PACKET_DATA_SIZE];
    my_group_num = htons(my_group_num);
    packet[0] = PACKET_ID_ONLINE_PACKET;
    memcpy(packet + 1, &my_group_num, sizeof(uint16_t));
    memcpy(packet + 1 + sizeof(uint16_t), identifier, GROUP_IDENTIFIER_LENGTH);
    return write_cryptpacket(friendconn_net_crypto(fr_c), friend_connection_crypt_connection_id(fr_c, (int)friendcon_id), packet,
                             sizeof(packet), 0) != -1;
}

static int handle_packet_online(Group_Chats *g_c, aint friendcon_id, const uint8_t *data, uint16_t length)
{
    if (length < ONLINE_PACKET_DATA_SIZE) {
        return -1;
    }

    int groupnumber = get_group_num(g_c, data + sizeof(uint16_t));
    uint16_t other_groupnum;
    memcpy(&other_groupnum, data, sizeof(uint16_t));
    other_groupnum = ntohs(other_groupnum);

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    aint peer_index = -1, i;

    for (i = 0; i < g->numpeers; ++i) {
        if (g->peers[i].friendcon_id == friendcon_id) {
            peer_index = i;
            break;
        }
    }

    if (peer_index == -1) {
        return -1;
    }

    Group_Peer *peer = g->peers + peer_index;

    if (peer->group_number != other_groupnum) {
        peer->group_number = other_groupnum;
        send_peer_query(g_c, friendcon_id, other_groupnum);
    }

    bool in_close = false;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
        if (CLOSEST(g,i)) {
            Group_Peer *peer_from = g->peers + g->closest_peers[i];

            if (peer_from->friendcon_id == friendcon_id) {
                peer_from->need_send_peers = true;
            }

            if (g->closest_peers[i] == peer_index) {
                in_close = true;
            }
        }
    }

    if (!in_close) {
        send_packet_online(g_c->fr_c, friendcon_id, (uint16_t)groupnumber, g->identifier);
    }

    return 0;
}

static void send_peer_kill(Group_Chats *g_c, aint friendcon_id, uint16_t other_group_num)
{
    uint8_t packet[1];
    packet[0] = PEER_KILL_ID;
    send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, packet, sizeof(packet));
}


static void send_peer_query(const Group_Chats *g_c, aint friendcon_id, uint16_t other_group_num)
{
    uint8_t packet[1];
    packet[0] = PEER_QUERY_ID;
    send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, packet, sizeof(packet));
}

static void send_peers(Group_Chats *g_c, aint groupnumber, int friendcon_id, uint16_t other_group_num)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    uint8_t packet[MAX_CRYPTO_DATA_SIZE - (1 + sizeof(uint16_t))];
    packet[0] = PEER_RESPONSE_ID;
    uint8_t *p = packet + 1;

    size_t sent = 0;
    size_t i;

    for (i = 0; i < g->numpeers; ++i) {

        if (g->peers[i].gid < 0) {
            continue;
        }

        if ((p - packet) + sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE * 2 + 1 + g->peers[i].nick_len > sizeof(packet)) {
            if (send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, packet,
                                       (p - packet))) {
                sent = i;
            } else {
                return;
            }

            p = packet + 1;
        }

        uint16_t peer_gid = htons(g->peers[i].gid);
        memcpy(p, &peer_gid, sizeof(peer_gid));
        p += sizeof(peer_gid);
        memcpy(p, g->peers[i].real_pk, CRYPTO_PUBLIC_KEY_SIZE);
        p += CRYPTO_PUBLIC_KEY_SIZE;
        memcpy(p, g->peers[i].temp_pk, CRYPTO_PUBLIC_KEY_SIZE);
        p += CRYPTO_PUBLIC_KEY_SIZE;
        *p = g->peers[i].nick_len;
        p += 1;
        memcpy(p, g->peers[i].nick, g->peers[i].nick_len);
        p += g->peers[i].nick_len;
    }

    if (sent != i) {
        if (send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, packet,
                                   (p - packet))) {
            sent = i;
        }
    }

    if (g->title_len) {

        uint8_t Packet[1 + MAX_NAME_LENGTH];
        Packet[0] = PEER_TITLE_ID;
        memcpy(Packet + 1, g->title, g->title_len);
        send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, other_group_num, Packet, 1 + g->title_len);
    }
}

static void accept_peers_list(Group_c *g, aint groupnumber, const uint8_t *data, aint length)
{
    if (length == 0) {
        return;
    }

    const uint8_t *d = data;

    while ((unsigned)(length - (d - data)) >= sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE * 2 + 1) {
        uint16_t peer_gid;
        memcpy(&peer_gid, d, sizeof(peer_gid));
        peer_gid = ntohs(peer_gid);
        d += sizeof(uint16_t);
        aint peer_index = addpeer(g, groupnumber, d, d + CRYPTO_PUBLIC_KEY_SIZE, peer_gid);

        if (peer_index == -1) {
            return;
        }

        d += CRYPTO_PUBLIC_KEY_SIZE * 2;
        uint8_t name_length = *d;
        d += 1;

        if (name_length > (length - (d - data)) || name_length > MAX_NAME_LENGTH) {
            return;
        }

        setnick(g, peer_index, d, name_length);
        d += name_length;
    }

    return;
}

static bool self_peer_gid_collision(Group_c *g)
{
    size_t i, me = ~0;
    int my_gid = 0;

    for (i = 0; i < g->numpeers; ++i) {

        if (id_equal(g->real_pk, g->peers[i].real_pk)) {
            if (g->peers[i].gid < 0) {
                return false;
            }

            me = i;
            my_gid = g->peers[i].gid;
            break;
        }
    }

    for (i = 0; i < g->numpeers; ++i) {

        if (me != i && my_gid == g->peers[i].gid) {
            return true;
        }
    }

    return false;
}

static void handle_direct_packet(Group_Chats *g_c, aint groupnumber, const uint8_t *data, aint length,
                                 aint peer_index)
{
    if (length == 0) {
        return;
    }

    switch (data[0]) {
        case PEER_KILL_ID: {
            Group_c *g = get_group_c(g_c, groupnumber);

            if (!g) {
                return;
            }

            kill_friend_connection(g_c->fr_c, g->peers[peer_index].friendcon_id);
            g->peers[peer_index].friendcon_id = -1;
            g->peers[peer_index].connected = false;
            g->peers[peer_index].group_number = 0xffff;
        }

        break;

        case PEER_QUERY_ID: {
            Group_c *g = get_group_c(g_c, groupnumber);

            if (!g) {
                return;
            }

            Group_Peer *peer = g->peers + peer_index;

            if (really_connected(peer)) {
                send_peers(g_c, groupnumber, peer->friendcon_id, peer->group_number);
                send_peer_nums(g_c, groupnumber, peer->friendcon_id, peer->group_number);
                peer->need_send_peers = false;
            }
        }

        break;

        case PEER_RESPONSE_ID: {

            size_t old_peers_num = 0;
            int self_peer_gid = 0;
            Group_c *g = get_group_c(g_c, groupnumber);

            if (g) {
                self_peer_gid = get_self_peer_gid(g);
                old_peers_num = g->numpeers;

                g->keep_leave = false;
                g->disable_auto_join = false;
                g->join_mode = false;

                accept_peers_list(g, groupnumber, data + 1, length - 1);

                if (g->numpeers > old_peers_num) {
                    g->need_send_name = true;
                    need_send_peers(g);
                }

                if (self_peer_gid != get_self_peer_gid(g)) {
                    /* disallow change self peer gid by remote */

                    change_self_peer_gid(g_c, groupnumber, self_peer_gid);

                    if (self_peer_gid_collision(g)) {
                        change_self_peer_gid(g_c, groupnumber, find_new_peer_gid(g));
                    }

                    if (self_peer_gid >= 0) {
                        group_new_peer_send(g_c, groupnumber, self_peer_gid, g->real_pk, dht_get_self_public_key(g_c->m->dht));
                    }
                }
            }
        }

        break;

        case PEER_TITLE_ID: {
            Group_c *g = get_group_c(g_c, groupnumber);

            if (g) {
                settitle(g, peer_index, data + 1, length - 1);
            }
        }

        break;

        case PEER_GROUP_NUM_ID: {
            if (length < (CRYPTO_PUBLIC_KEY_SIZE + sizeof(uint16_t) + 1)) {
                return;
            }

            Group_c *g = get_group_c(g_c, groupnumber);

            if (g) {
                --length;
                ++data;

                for (; length > (CRYPTO_PUBLIC_KEY_SIZE + sizeof(uint16_t));
                        length -= (CRYPTO_PUBLIC_KEY_SIZE + sizeof(uint16_t)),
                        data += (CRYPTO_PUBLIC_KEY_SIZE + sizeof(uint16_t))) {

                    set_peer_groupnum(g, data, ntohs(*(uint16_t *)(data + CRYPTO_PUBLIC_KEY_SIZE)));
                }


            }

        }

        break;
    }
}

/* Send message to all close except receiver (if receiver isn't -1)
 * NOTE: this function appends the group chat number to the data passed to it.
 *
 * return number of messages sent.
 */
static aint send_message_all_close(const Group_Chats *g_c, aint groupnumber, const uint8_t *data,
                                   uint16_t length, aint except_peer)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return 0;
    }

    aint i, sent = 0;

    for (i = 0; i < g->numpeers; ++i) {

        if (i == except_peer) {
            continue;
        }

        Group_Peer *peer = g->peers + i;

        if (!really_connected(peer)) {
            continue;
        }

        if (send_packet_group_peer(g_c->fr_c, peer->friendcon_id, PACKET_ID_MESSAGE_CONFERENCE, peer->group_number, data,
                                   length)) {
            ++sent;
        }
    }

    return sent;
}

/* Send lossy message to all close except receiver (if receiver isn't -1)
 * NOTE: this function appends the group chat number to the data passed to it.
 *
 * return number of messages sent.
 */
static size_t send_lossy_all_close(const Group_Chats *g_c, aint groupnumber, const uint8_t *data, aint length,
                                   aint receiver)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return 0;
    }

    size_t i, sent = 0, num_closest = 0;

    for (i = 0; i < g->numpeers; ++i) {

        if (!really_connected(g->peers + i) || (aint)i == receiver) {
            continue;
        }

        size_t k;

        for (k = 0; k < DESIRED_CLOSE_CONNECTIONS; ++k) {

            if (CLOSEST(g,k) && g->closest_peers[k] == i) {
                k = DESIRED_CLOSE_CONNECTIONS + 100;
                ++num_closest;
                break;
            }
        }

        if (k == DESIRED_CLOSE_CONNECTIONS + 100) {
            continue;
        }


        if (send_lossy_group_peer(g_c->fr_c, g->peers[i].friendcon_id, PACKET_ID_LOSSY_CONFERENCE, g->peers[i].group_number,
                                  data,
                                  length)) {
            ++sent;
        }
    }

    if (!num_closest) {
        return sent;
    }

    aint to_send = -1;
    uint64_t comp_val_old = ~0;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {

        if (!CLOSEST(g,i)) {
            continue;
        }

        aint peer_index = g->closest_peers[i];

        if (peer_index == receiver) {
            continue;
        }

        Group_Peer *peer = g->peers + peer_index;


        if (!really_connected(peer)) {
            continue;
        }

        uint64_t comp_val = calculate_comp_value(g->real_pk, peer->real_pk);

        if (comp_val < comp_val_old) {
            to_send = peer_index;
            comp_val_old = comp_val;
        }
    }

    if (to_send >= 0 && send_lossy_group_peer(g_c->fr_c, g->peers[to_send].friendcon_id, PACKET_ID_LOSSY_CONFERENCE,
            g->peers[to_send].group_number, data, length)) {
        ++sent;
    }

    aint to_send_other = -1;
    comp_val_old = ~0;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {

        if (!CLOSEST(g,i)) {
            continue;
        }

        aint peer_index = g->closest_peers[i];

        if (peer_index == receiver) {
            continue;
        }

        Group_Peer *peer = g->peers + peer_index;

        if (!really_connected(peer)) {
            continue;
        }

        uint64_t comp_val = calculate_comp_value(peer->real_pk, g->real_pk);

        if (comp_val < comp_val_old) {
            to_send_other = peer_index;
            comp_val_old = comp_val;
        }
    }

    if (to_send_other == to_send) {
        return sent;
    }

    if (to_send_other >= 0
            && send_lossy_group_peer(g_c->fr_c, g->peers[to_send_other].friendcon_id, PACKET_ID_LOSSY_CONFERENCE,
                                     g->peers[to_send_other].group_number, data, length)) {
        ++sent;
    }

    return sent;
}

static aint group_packet_index(aint msg_id)
{
    switch (msg_id) {
        case GROUP_MESSAGE_PING_ID:
            return 0;

        case GROUP_MESSAGE_UNSUBSCRIBE_ID:
            return 1;

        case GROUP_MESSAGE_NICKNAME_ID:
            return 2;

        case GROUP_MESSAGE_NEW_PEER_ID:
            return 3;

        case GROUP_MESSAGE_KILL_PEER_ID:
            return 4;

        case GROUP_MESSAGE_NAME_ID:
            return 5;

        case GROUP_MESSAGE_TITLE_ID:
            return 6;

        case PACKET_ID_MESSAGE:
            return 7;

        case PACKET_ID_ACTION:
            return 8;
    }

    return -1;
}


/* Send data of len with message_id to groupnumber.
 *
 * return number of peers it was sent to on success.
 * return -1 if groupnumber is invalid.
 * return -2 if message is too long.
 * return -3 if we are not connected to the group.
 * reutrn -4 if message failed to send.
 * reutrn -5 if unknown message_id.
 */
static aint send_message_group(const Group_Chats *g_c, aint groupnumber, uint8_t message_id, const uint8_t *data,
                               aint len)
{

    if (len > MAX_GROUP_MESSAGE_DATA_LEN) {
        return -2;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    Group_Peer *self_peer = get_self_peer(g);

    int source_peer_gid = self_peer ? self_peer->gid : -1;

    if (source_peer_gid < 0) {
        return -3;
    }

    aint pindex = group_packet_index(message_id);

    if (pindex < 0) {
        return -5;
    }

    self_peer->last_message_number[pindex] = g->message_number;

    ++g->message_number;

    if (!g->message_number) {
        ++g->message_number;
    }

    uint32_t msgnum = g->message_number;

    uint8_t packet[MAX_GROUP_MESSAGE_DATA_LEN + sizeof(uint16_t) + sizeof(uint32_t) + 1];
    size_t packet_len = sizeof(uint16_t) + sizeof(uint32_t) + 1 + len;
    uint16_t peer_gid = htons(source_peer_gid);
    memcpy(packet, &peer_gid, sizeof(peer_gid));

    uint32_t message_num = htonl(msgnum);
    memcpy(packet + sizeof(uint16_t), &message_num, sizeof(message_num));

    packet[sizeof(uint16_t) + sizeof(uint32_t)] = message_id;

    if (len) {
        memcpy(packet + sizeof(uint16_t) + sizeof(uint32_t) + 1, data, len);
    }

    aint ret = send_message_all_close(g_c, groupnumber, packet, (uint16_t)packet_len, -1);

    return (ret == 0) ? -4 : ret;
}

/* send a group message
 * return 0 on success
 * see: send_message_group() for error codes.
 */
int group_message_send(const Group_Chats *g_c, int groupnumber, const uint8_t *message, uint16_t length)
{
    aint ret = send_message_group(g_c, groupnumber, PACKET_ID_MESSAGE, message, length);

    if (ret > 0) {
        return 0;
    }

    return (int)ret;
}

/* send a group action
 * return 0 on success
 * see: send_message_group() for error codes.
 */
int group_action_send(const Group_Chats *g_c, int groupnumber, const uint8_t *action, uint16_t length)
{
    aint ret = send_message_group(g_c, groupnumber, PACKET_ID_ACTION, action, length);

    if (ret > 0) {
        return 0;
    }

    return (int)ret;
}

/* High level function to send custom lossy packets.
 *
 * return -1 on failure.
 * return 0 on success.
 */
int send_group_lossy_packet(const Group_Chats *g_c, int groupnumber, const uint8_t *data, uint16_t length)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    uint8_t packet[MAX_CRYPTO_DATA_SIZE];
    size_t plen = sizeof(uint16_t) * 2 + length;

    int self_peer_gid = get_self_peer_gid(g);

    if (self_peer_gid < 0) {
        return -1;
    }

    uint16_t peer_gid = htons((uint16_t)self_peer_gid);
    memcpy(packet, &peer_gid, sizeof(uint16_t));
    uint16_t message_num = htons(g->lossy_message_number);
    memcpy(packet + sizeof(uint16_t), &message_num, sizeof(uint16_t));
    memcpy(packet + sizeof(uint16_t) * 2, data, length);

    if (send_lossy_all_close(g_c, groupnumber, packet, (uint16_t)plen, -1) == 0) {
        return -1;
    }

    ++g->lossy_message_number;
    return 0;
}

static void handle_message_packet_group(Group_Chats *g_c, int groupnumber, const uint8_t *data, uint16_t length,
                                        aint peer_index_from, void *userdata)
{
    if (length < sizeof(uint16_t) + sizeof(uint32_t) + 1) {
        return;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    uint16_t from_peer_gid = ntohs(*(uint16_t *)data);
    aint index = get_peer_index(g, from_peer_gid);

    uint8_t msg_id = data[sizeof(uint16_t) + sizeof(uint32_t)];

    aint pindex = group_packet_index(msg_id);

    if (pindex < 0) {
        return;
    }

    if (index == -1) {

        if (really_connected(g->peers + peer_index_from)) {
            send_peer_query(g_c, g->peers[peer_index_from].friendcon_id, g->peers[peer_index_from].group_number);
        }

        if (msg_id != GROUP_MESSAGE_NEW_PEER_ID) {
            return; /* this packet is very important to autojoin and compatibility, so we proceed id even no source peer known */
        }
    }

    bool allow_resend = false;

    if (index >= 0) {

        if (!id_equal(g->real_pk, g->peers[index].real_pk)) {
            allow_resend = true;
        }

        uint32_t message_number;
        memcpy(&message_number, data + sizeof(uint16_t), sizeof(message_number));
        message_number = ntohl(message_number);

        if (g->peers[index].last_message_number[pindex] > 0 && message_number == g->peers[index].last_message_number[pindex]) {
            return;
        }

        if (message_number < g->peers[index].last_message_number[pindex]) {
            /* accept only increasing message numbers to avoid infinite loops */
            return;
        }

        g->peers[index].last_message_number[pindex] = message_number;
    }

    const uint8_t *msg_data = data + sizeof(uint16_t) + sizeof(uint32_t) + 1;
    uint16_t msg_data_len = length - (sizeof(uint16_t) + sizeof(uint32_t) + 1);

    switch (msg_id) {
        case GROUP_MESSAGE_PING_ID: {
            /* absolutely no matter size of ping packet */
            Group_Peer *peer = g->peers + index;
            peer->last_recv = unix_time();

            if (peer->nick_len == 0 && peer->gid >= 0) {
                /* empty nick */
                nick_request_send(g_c, groupnumber, peer->gid);
            }

            if (peer->keep_connection) {
                --peer->keep_connection;
            }
        }
        break;

        case GROUP_MESSAGE_UNSUBSCRIBE_ID: {

            if (msg_data_len < (GROUP_IDENTIFIER_LENGTH + CRYPTO_PUBLIC_KEY_SIZE)) {
                return;
            }

            unsubscribe_peer(g_c, msg_data + 1, msg_data + GROUP_IDENTIFIER_LENGTH, (UnsubscribeType)msg_data[0]);
        }
        break;

        case GROUP_MESSAGE_NEW_PEER_ID: {

            if (msg_data_len < GROUP_MESSAGE_NEW_PEER_LENGTH) {
                return;
            }

            uint16_t new_peer_gid;
            memcpy(&new_peer_gid, msg_data, sizeof(uint16_t));
            new_peer_gid = ntohs(new_peer_gid);

            if (from_peer_gid == new_peer_gid) {
                allow_resend = false;
            }

            const uint8_t *real_pk = msg_data + sizeof(uint16_t);
            const uint8_t *temp_pk = real_pk + CRYPTO_PUBLIC_KEY_SIZE;

            if (id_equal(g->real_pk, real_pk)) {

                allow_resend = false;
                g->need_send_name = true;
                g->keep_leave = false;
                g->disable_auto_join = false;
                g->join_mode = false;
            }

            /*aint peer_index =*/ addpeer(g, groupnumber, real_pk, temp_pk, new_peer_gid);

            int self_peer_gid = get_self_peer_gid(g);

            if (self_peer_gid >= 0 && self_peer_gid_collision(g)) {
                self_peer_gid = find_new_peer_gid(g);
                change_self_peer_gid(g_c, groupnumber, self_peer_gid);
                group_new_peer_send(g_c, groupnumber, (uint16_t)self_peer_gid, g->real_pk, dht_get_self_public_key(g_c->m->dht));
            }
        }
        break;

        case GROUP_MESSAGE_NICKNAME_ID: {
            if (msg_data_len < sizeof(uint16_t)) {
                return;
            }

            uint16_t gid;
            memcpy(&gid, msg_data, sizeof(uint16_t));
            gid = ntohs(gid);

            Group_Peer *self = get_self_peer(g);

            if (self && gid == self->gid) {
                self->nick_changed = true;
                g->nick_changed = true;
            }

        }
        break;

        case GROUP_MESSAGE_KILL_PEER_ID: {

            if (msg_data_len < GROUP_MESSAGE_KILL_PEER_LENGTH) {
                return;
            }

            uint16_t kill_peer_number;
            memcpy(&kill_peer_number, msg_data, sizeof(uint16_t));
            kill_peer_number = ntohs(kill_peer_number);

            if (kill_peer_number == from_peer_gid) {

                Group_c *g = get_group_c(g_c, groupnumber);

                if (g) {
                    unsubscribe_peer(g_c, g->identifier + 1, g->peers[index].real_pk, UNS_TEMP);
                }

                delpeer(g_c, groupnumber, index);
            }
        }
        break;

        case GROUP_MESSAGE_NAME_ID: {
            if (setnick(g, index, msg_data, msg_data_len) == -1) {
                return;
            }
        }
        break;

        case GROUP_MESSAGE_TITLE_ID: {
            if (settitle(g, index, msg_data, msg_data_len) == -1) {
                return;
            }
        }
        break;

        case PACKET_ID_MESSAGE: {
            if (msg_data_len == 0) {
                return;
            }

            uint8_t newmsg[MAX_CRYPTO_DATA_SIZE];
            memcpy(newmsg, msg_data, msg_data_len);
            newmsg[msg_data_len] = 0;

            if (g_c->message_callback) {

                int index_in_list = find_peer_index_in_list(g, index);

                if (index_in_list >= 0) {
                    g_c->message_callback(g_c->m, (uint32_t)groupnumber, (uint32_t)index_in_list, 0, newmsg, msg_data_len, userdata);
                }
            }

            break;
        }

        case PACKET_ID_ACTION: {
            if (msg_data_len == 0) {
                return;
            }

            uint8_t newmsg[MAX_CRYPTO_DATA_SIZE];
            memcpy(newmsg, msg_data, msg_data_len);
            newmsg[msg_data_len] = 0;

            if (g_c->message_callback) {
                int index_in_list = find_peer_index_in_list(g, index);

                if (index_in_list >= 0) {
                    g_c->message_callback(g_c->m, (uint32_t)groupnumber, (uint32_t)index_in_list, 1, newmsg, msg_data_len, userdata);
                }
            }

            break;
        }

        default:
            return;
    }

    if (allow_resend) {
        send_message_all_close(g_c, groupnumber, data, length, msg_id == PACKET_ID_MESSAGE
                               || msg_id == PACKET_ID_ACTION ? -1 : peer_index_from);
    }
}

static int handle_packet(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)object;

    if (length < 1 + sizeof(uint16_t) + 1) {
        return -1;
    }

    if (data[0] == PACKET_ID_ONLINE_PACKET) {
        return handle_packet_online(g_c, friendcon_id, data + 1, length - 1);
    }

    if (data[0] != PACKET_ID_DIRECT_CONFERENCE && data[0] != PACKET_ID_MESSAGE_CONFERENCE) {
        return -1;
    }

    uint16_t groupnumber;
    memcpy(&groupnumber, data + 1, sizeof(uint16_t));
    groupnumber = ntohs(groupnumber);
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    aint peer_index = -1, i;

    for (i = 0; i < g->numpeers; ++i) {
        if (g->peers[i].friendcon_id == friendcon_id) {
            peer_index = i;
            break;
        }
    }

    if (peer_index == -1) {
        return -1;
    }

    switch (data[0]) {
        case PACKET_ID_DIRECT_CONFERENCE: {
            handle_direct_packet(g_c, groupnumber, data + 1 + sizeof(uint16_t), length - (1 + sizeof(uint16_t)), peer_index);
            break;
        }

        case PACKET_ID_MESSAGE_CONFERENCE: {
            handle_message_packet_group(g_c, groupnumber, data + 1 + sizeof(uint16_t), length - (1 + sizeof(uint16_t)), peer_index,
                                        userdata);
            break;
        }

        default: {
            return 0;
        }
    }

    return 0;
}

/* Did we already receive the lossy packet or not.
 *
 * return -1 on failure.
 * return 0 if packet was not received.
 * return 1 if packet was received.
 *
 * TODO(irungentoo): test this
 */
static aint lossy_packet_not_received(Group_c *g, aint peer_index, uint16_t message_number)
{
    if (peer_index == -1) {
        return -1;
    }

    Group_Peer *peer = g->peers + peer_index;
    Group_Peer_Lossy *lossy = peer->lossy;

    if (!lossy) {
        lossy = (Group_Peer_Lossy *)calloc(1, sizeof(Group_Peer_Lossy));
        peer->lossy = lossy;
    }

    if (lossy->bottom_lossy_number == lossy->top_lossy_number) {
        lossy->top_lossy_number = message_number;
        lossy->bottom_lossy_number = (message_number - MAX_LOSSY_COUNT) + 1;
        lossy->recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
        return 0;
    }

    if ((uint16_t)(message_number - lossy->bottom_lossy_number) < MAX_LOSSY_COUNT) {
        if (lossy->recv_lossy[message_number % MAX_LOSSY_COUNT]) {
            return 1;
        }

        lossy->recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
        return 0;
    }

    if ((uint16_t)(message_number - lossy->bottom_lossy_number) > (1 << 15)) {
        return -1;
    }

    uint16_t top_distance = message_number - lossy->top_lossy_number;

    if (top_distance >= MAX_LOSSY_COUNT) {
        crypto_memzero(lossy->recv_lossy, sizeof(lossy->recv_lossy));
        lossy->top_lossy_number = message_number;
        lossy->bottom_lossy_number = (message_number - MAX_LOSSY_COUNT) + 1;
        lossy->recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
        return 0;
    }

    if (top_distance < MAX_LOSSY_COUNT) {
        size_t i;

        for (i = lossy->bottom_lossy_number; i != (lossy->bottom_lossy_number + top_distance);
                ++i) {
            lossy->recv_lossy[i % MAX_LOSSY_COUNT] = 0;
        }

        lossy->top_lossy_number = message_number;
        lossy->bottom_lossy_number = (message_number - MAX_LOSSY_COUNT) + 1;
        lossy->recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
        return 0;
    }

    return -1;
}

static int handle_lossy(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)object;

    if (length < 1 + sizeof(uint16_t) * 3 + 1) {
        return -1;
    }

    if (data[0] != PACKET_ID_LOSSY_CONFERENCE) {
        return -1;
    }

    uint16_t groupnumber, peer_gid, message_number;
    memcpy(&groupnumber, data + 1, sizeof(uint16_t));
    memcpy(&peer_gid, data + 1 + sizeof(uint16_t), sizeof(uint16_t));
    memcpy(&message_number, data + 1 + sizeof(uint16_t) * 2, sizeof(uint16_t));
    groupnumber = ntohs(groupnumber);
    peer_gid = ntohs(peer_gid);
    message_number = ntohs(message_number);

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    aint peer_index_from = -1, i;

    for (i = 0; i < g->numpeers; ++i) {
        if (g->peers[i].friendcon_id == friendcon_id) {
            peer_index_from = i;
            break;
        }
    }

    if (peer_index_from == -1) {
        return -1;
    }

    if ((int)peer_gid == get_self_peer_gid(g)) {
        return -1;
    }

    aint peer_index = get_peer_index(g, peer_gid);

    if (peer_index == -1) {
        return -1;
    }

    if (lossy_packet_not_received(g, peer_index, message_number)) {
        return -1;
    }

    const uint8_t *lossy_data = data + 1 + sizeof(uint16_t) * 3;
    uint16_t lossy_length = length - (1 + sizeof(uint16_t) * 3);
    uint8_t message_id = lossy_data[0];
    ++lossy_data;
    --lossy_length;

    int index_in_list = find_peer_index_in_list(g, peer_index);

    if (index_in_list < 0) {
        return -1;
    }

    /*
    if (g_c->lossy_packethandlers[message_id].function) {
        if (g_c->lossy_packethandlers[message_id].function(g->object, (int)groupnumber, index_in_list, g->peers[peer_index].object,
                lossy_data, lossy_length) == -1) {
            return -1;
        }
    } else {
        return -1;
    }
    */

    if (192 /*GROUP_AUDIO_PACKET_ID*/ == message_id && g_c->lossy_packethandler) {
        if (g_c->lossy_packethandler(g->object, (int)groupnumber, index_in_list, g->peers[peer_index].object,
                                     lossy_data, lossy_length) == -1) {
            return -1;
        }
    } else {
        return -1;
    }


    send_lossy_all_close(g_c, groupnumber, data + 1 + sizeof(uint16_t), length - (1 + sizeof(uint16_t)), peer_index_from);
    return 0;
}

/* Set the object that is tied to the group chat.
 *
 * return 0 on success.
 * return -1 on failure
 */
int group_set_object(const Group_Chats *g_c, int groupnumber, void *object)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->object = object;
    return 0;
}

/* Set the object that is tied to the group peer.
 *
 * return 0 on success.
 * return -1 on failure
 */
int group_peer_set_object(const Group_Chats *g_c, int groupnumber, int peernumber, void *object)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return -1;
    }

    if (g->peers[peernumber].object && g->peer_on_leave) {
        g->peer_on_leave(g->object, groupnumber, g->peers[peernumber].object);
    }

    g->peers[peernumber].object = object;
    return 0;
}

/* Return the object tide to the group chat previously set by group_set_object.
 *
 * return NULL on failure.
 * return object on success.
 */
void *group_get_object(const Group_Chats *g_c, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return NULL;
    }

    return g->object;
}

/* Return the object tide to the group chat peer previously set by group_peer_set_object.
 *
 * return NULL on failure.
 * return object on success.
 */
void *group_peer_get_object(const Group_Chats *g_c, int groupnumber, int peernumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return NULL;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return NULL;
    }

    return g->peers[peernumber].object;
}

static int64_t deltatime(uint64_t t1, uint64_t t2)
{
    return (int64_t)(t1 - t2);
}

static bool possible_groupnum(Group_Chats *g_c, Group_c *ig, uint16_t gn, const uint8_t *for_pk)
{
    size_t i;

    for (i = 0; i < g_c->num_chats; ++i) {

        Group_c *g = get_group_c(g_c, i);

        if (g == ig) {
            continue;
        }

        if (g) {

            aint peer_index = peer_in_chat(g, for_pk);

            if (peer_index >= 0 && g->peers[peer_index].group_number == gn) {
                /* same peer already in gn group, so, current group has other gn value */
                return false;
            }
        }
    }

    return true;
}

static Group_Join_Peer *keep_join_mode(Group_Chats *g_c, Group_c *g, uint64_t ct)
{
    if (g->keep_join_index >= 0) {

        if (g->keep_join_index >= (int)g->numjoinpeers) {
            g->keep_join_index = 0;
        }

        int next_time = 333;
        Group_Join_Peer *jd = g->joinpeers + g->keep_join_index;

        int friend_index = getfriend_id(g_c->m, jd->real_pk);
        int friendcon_id = friend_index >= 0 ? getfriendcon_id(g_c->m, friend_index) : -1;

        if (friendcon_id >= 0 && friend_con_connected(g_c->fr_c, friendcon_id) == FRIENDCONN_STATUS_CONNECTED
                && !jd->unsubscribed) {

            next_time = 5000;

            int mcount = g->numpeers; /* 1 - me */

            bool present = false;
            int m = 0;

            for (; m < mcount; ++m) {
                if (id_equal(g->peers[m].real_pk, jd->real_pk)) {
                    next_time = 1000;
                    present = true;
                    break;
                }
            }

            if (!present) {
                /* wow, member is online, but not in conference? restore! */
                if (jd->fails >= MAX_FAILED_JOIN_ATTEMPTS) {
                    jd->fails = 0;
                    jd->online = true;
                }

                jd->next_try_time = ct;
                return jd;
            }
        }

        g->next_join_check_time = ct + next_time;
        ++g->keep_join_index;
    }

    return NULL;
}

typedef struct {
    uint64_t ct;

    Group_Chats *g_c;
    Group_c *g;
    Group_Join_Peer *j;

    aint gn;
    aint jpi;

} jp_iterator;

static Group_Join_Peer *jp_iterator_next(jp_iterator *itr)
{
    for (;;) {
        if (itr->gn >= itr->g_c->num_chats) {
            itr->g = NULL;
            itr->j = NULL;
            return NULL;
        }

        if (itr->g == NULL) {
            itr->jpi = 0;
            itr->g = get_group_c(itr->g_c, itr->gn);

            if (itr->g == NULL) {
                ++itr->gn;
                continue;
            }
        }

        if (itr->jpi >= itr->g->numjoinpeers || !itr->g->join_mode || itr->g->disable_auto_join || itr->g->keep_leave
                || (itr->ct != 0 && deltatime(itr->ct, itr->g->next_join_check_time) < 0)) {
            itr->g = NULL;
            itr->jpi = 0;
            ++itr->gn;
            continue;
        }

        itr->j = itr->g->joinpeers + itr->jpi;
        ++itr->jpi;
        return itr->j;
    }

}

static Group_Join_Peer *jp_iterator_setup(jp_iterator *itr, Group_Chats *g_c, uint64_t t)
{
    itr->ct = t;
    itr->g_c = g_c;
    itr->g = NULL;
    itr->gn = 0;
    itr->jpi = 0;
    itr->j = NULL;
    return jp_iterator_next(itr);
}

static void set_next_join_try(Group_Chats *g_c, const uint8_t *real_pk, uint64_t t)
{
    for (size_t i = 0; i < g_c->num_chats; ++i) {
        Group_c *g = get_group_c(g_c, i);

        if (g && g->join_mode) {

            for (size_t j = 0; j < g->numjoinpeers; ++j) {
                if (id_equal(g->joinpeers[j].real_pk, real_pk)) {
                    g->joinpeers[j].next_try_time = t;
                    break;
                }
            }
        }
    }
}

static void restore_conference(Group_Chats *g_c)
{
    uint64_t ct = current_time_monotonic();

    Group_Join_Peer *jd = NULL;
    Group_c *g = NULL;

    aint minnum = MAX_FAILED_JOIN_ATTEMPTS;

    bool at_max = false;
    bool on_try = false;

    jp_iterator jpi;

    for (Group_Join_Peer *j = jp_iterator_setup(&jpi, g_c, ct); j; j = jp_iterator_next(&jpi)) {

        if (j->unsubscribed) {
            j->fails = MAX_FAILED_JOIN_ATTEMPTS;
        }

        if (j->fails >= MAX_FAILED_JOIN_ATTEMPTS) {
            at_max = true;
            continue;
        }

        int friend_index = getfriend_id(g_c->m, j->real_pk);

        if (friend_index >= 0) {

            int friendcon_id = getfriendcon_id(g_c->m, friend_index);

            if (friendcon_id >= 0 && friend_con_connected(g_c->fr_c, friendcon_id) == FRIENDCONN_STATUS_CONNECTED) {

                if (!j->online) {
                    j->online = true;
                    j->fails = 0;
                    j->next_try_time = ct + 3000; /* wait 3 sec */
                    jpi.g->next_join_check_time = ct + 333;
                }
            }
        } else {
            j->fails = MAX_FAILED_JOIN_ATTEMPTS; /* not a friend - skip */
            at_max = true;
            continue;
        }

        on_try = true;

        if (deltatime(ct, j->next_try_time) < 0) {
            continue;
        }

        if (j->fails < minnum) {
            jd = j;
            g = jpi.g;
            minnum = j->fails;
        }

    }

    if (jd == NULL) {

        if (at_max && !on_try) {

            /* now restart join process */

            for (Group_Join_Peer *j = jp_iterator_setup(&jpi, g_c, 0); j; j = jp_iterator_next(&jpi)) {

                j->online = false;
                j->unsubscribed = false;
                j->fails = 0;
                j->next_try_time = ct;

                jpi.g->next_join_check_time = ct + 20000; /* next attempt in 20 sec */
            }

            return;
        }

        if (!at_max && !on_try) {

            /* keep_join_index */

            size_t i;

            for (i = 0; i < g_c->num_chats; ++i) {
                g = get_group_c(g_c, i);

                if (g) {

                    if (deltatime(ct, g->next_join_check_time) < 0) {
                        continue;
                    }

                    g->next_join_check_time = ct + 5000;

                    if (!g->join_mode && !g->keep_leave && !g->disable_auto_join && g->numjoinpeers) {

                        if (g->keep_join_index < 0) {
                            g->keep_join_index = 0;
                        }

                        jd = keep_join_mode(g_c, g, ct);

                        if (jd) {
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!jd) {
        return;
    }

    int friend_index = jd->online ? getfriend_id(g_c->m, jd->real_pk) : -1;

    if (friend_index >= 0) {

        while (!possible_groupnum(g_c, g, jd->fails, jd->real_pk)) {
            ++jd->fails;
        };

        uint8_t invite_data[GROUP_IDENTIFIER_LENGTH + sizeof(uint16_t)];

        *(uint16_t *)invite_data = htons(jd->fails);

        memcpy(invite_data + 2, g->identifier, GROUP_IDENTIFIER_LENGTH);


        g->auto_join = true;

        join_groupchat(g_c, friend_index, g->identifier[0], invite_data, sizeof(invite_data));

        g->auto_join = false;

        ++jd->fails;

        set_next_join_try(g_c, jd->real_pk, ct +
                          20000); /* next try in 20 sec!!! It is important to wait for answer to this try */

    } else {
        ++jd->fails;
    }

    g->next_join_check_time = ct + 333;
}

static int ping_groupchat(Group_Chats *g_c, int groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (is_timeout(g->last_sent_ping, GROUP_PING_INTERVAL)) {
        if (group_ping_send(g_c, groupnumber) != -1) { /* Ping */
            g->last_sent_ping = unix_time();
        } else {
            /* no peers to ping */
            g->last_sent_ping = unix_time() - GROUP_PING_INTERVAL + 2;
        }

    }

    return 0;
}

static void groupchat_clear_timedout(Group_Chats *g_c, int groupnumber, void *userdata)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    size_t i, opc = 0, dp = 0;

    for (i = 0; i < g->numpeers; ++i) {

        if (g->peers[i].friendcon_id == -2 || id_equal(g->peers[i].real_pk, g->real_pk)) {
            continue;
        }

        ++opc;

        if (is_timeout(g->peers[i].last_recv, GROUP_PING_INTERVAL * 3)) {
            delpeer(g_c, groupnumber, i);
            ++dp;
        }
    }

    if (opc == dp && opc) {
        /* looks like lost connection to other peers */
        on_offline(g);
    }

}

/* Send current name (set in messenger) to all online groups.
 */
void send_name_all_groups(Group_Chats *g_c)
{
    size_t i;

    for (i = 0; i < g_c->num_chats; ++i) {

        if (is_groupnumber_valid(g_c, i)) {
            group_name_send(g_c, i, g_c->m->name, g_c->m->name_length);
        }

    }
}

/* Create new groupchat instance. */
Group_Chats *new_groupchats(Messenger *m)
{
    if (!m) {
        return NULL;
    }

    Group_Chats *temp = (Group_Chats *)calloc(1, sizeof(Group_Chats));

    if (temp == NULL) {
        return NULL;
    }

    temp->m = m;
    temp->fr_c = m->fr_c;
    m->conferences_object = temp;
    m_callback_conference_invite(m, &handle_friend_invite_packet);

    return temp;
}

/* main groupchats loop. */
void do_groupchats(Group_Chats *g_c, void *userdata)
{
    unsigned i;
    bool is_online = onion_connection_status(g_c->m->onion_c) != 0;

    if (!is_online && g_c->is_online) {
        /* to offline */

        for (i = 0; i < g_c->num_chats; ++i) {

            Group_c *g = g_c->chats + i;

            if (!g->live || g->disable_auto_join) {
                continue;
            }

            disconnect_conference(g_c, i, UNS_NONE);
            on_offline(g);
        }
    }

    g_c->is_online = is_online;

    if (!is_online) {
        return;
    }

    for (i = 0; i < g_c->num_chats; ++i) {

        Group_c *g = g_c->chats + i;

        if (!g->live) {
            continue;
        }

        if (g->disable_auto_join) {
            groupchat_clear_timedout(g_c, i, userdata);
            apply_changes_in_peers(g_c, i, userdata);
            continue;
        }

        apply_changes_in_peers(g_c, i, userdata);
        connect_to_closest(g_c, i, userdata);
        ping_groupchat(g_c, i);
        groupchat_clear_timedout(g_c, i, userdata);
    }

    restore_conference(g_c);     /* always do something to restore contacts */
}

/* Free everything related with group chats. */
void kill_groupchats(Group_Chats *g_c)
{
    unsigned i;

    for (i = 0; i < g_c->num_chats; ++i) {
        del_groupchat_internal(g_c, i, UNS_NONE);
    }

    m_callback_conference_invite(g_c->m, NULL);
    g_c->m->conferences_object = NULL;
    free(g_c);
}

/* Return the number of chats in the instance m.
 * You should use this to determine how much memory to allocate
 * for copy_chatlist.
 */
uint32_t count_chatlist(Group_Chats *g_c)
{
    uint32_t ret = 0;
    uint32_t i;

    for (i = 0; i < g_c->num_chats; i++) {
        if (g_c->chats[i].live) {
            ret++;
        }
    }

    return ret;
}

/* Copy a list of valid chat IDs into the array out_list.
 * If out_list is NULL, returns 0.
 * Otherwise, returns the number of elements copied.
 * If the array was too small, the contents
 * of out_list will be truncated to list_size. */
uint32_t copy_chatlist(Group_Chats *g_c, uint32_t *out_list, uint32_t list_size)
{
    if (!out_list) {
        return 0;
    }

    if (g_c->num_chats == 0) {
        return 0;
    }

    uint32_t i, ret = 0;

    for (i = 0; i < g_c->num_chats; ++i) {
        if (ret >= list_size) {
            break;  /* Abandon ship */
        }

        if (g_c->chats[i].live) {
            out_list[ret] = i;
            ret++;
        }
    }

    return ret;
}

uint32_t saved_conferences_size(const Messenger *m)
{
    Group_Chats *g_c = (Group_Chats *)m->conferences_object;

    size_t sz = sizeof(uint16_t);   // size of number of groupchats
    size_t i;

    for (i = 0; i < g_c->num_chats; ++i) {

        Group_c *g = g_c->chats + i;

        if (!g->live) {
            continue;
        }

        sz += GROUP_IDENTIFIER_LENGTH + 1 + 1 + sizeof(
                  uint16_t); /* +1 byte for options, +1 byte for title len, +2 bytes for count of joinpeers */
        sz += g->title_len; /* +1 byte for title len */
        sz += g->numjoinpeers * CRYPTO_PUBLIC_KEY_SIZE;
    }

    return (uint32_t)sz;
}

#define put16(v) { *((uint16_t *)data) = host_tolendian16( (uint16_t)(v) ); data += sizeof( uint16_t ); }
#define putbytes( p, sz ) {memcpy(data,p, sz); data += sz;}

void conferences_save(const Messenger *m, uint8_t *data)
{
    Group_Chats *g_c = (Group_Chats *)m->conferences_object;

    uint16_t *num = (uint16_t *)data;
    *num = 0;
    data += sizeof(uint16_t);

    size_t i;

    for (i = 0; i < g_c->num_chats; ++i) {

        Group_c *g = g_c->chats + i;

        if (!g->live) {
            continue;
        }

        putbytes(g->identifier, GROUP_IDENTIFIER_LENGTH);

        uint8_t options = 0;

        if (g->keep_leave) {
            options = 1;
        }

        *data = options;
        ++data;

        *data = g->title_len;
        ++data;
        putbytes(g->title, g->title_len);

        put16(g->numjoinpeers);

        size_t j;

        for (j = 0; j < g->numjoinpeers; ++j) {
            putbytes(g->joinpeers[j].real_pk, CRYPTO_PUBLIC_KEY_SIZE);
        }

        ++(*num);
    }

}

int conferences_load(Messenger *m, const uint8_t *data, uint32_t length)
{
    if (length < sizeof(uint16_t)) {
        return -1;
    }

    Group_Chats *g_c = (Group_Chats *)m->conferences_object;

    uint32_t i;

    for (i = 0; i < g_c->num_chats; ++i) {
        del_groupchat_internal(g_c, i, UNS_NONE);
    }

    g_c->num_chats = 0;

    size_t numgchats = lendian_to_host16(*(uint16_t *)data);
    data += sizeof(uint16_t), length -= sizeof(uint16_t);

    for (i = 0; i < numgchats; ++i) {

        if (length < GROUP_IDENTIFIER_LENGTH + 4) {
            return -1;
        }

        int grounumber = add_groupchat(g_c, *data, data + 1);
        data += GROUP_IDENTIFIER_LENGTH;
        length -= GROUP_IDENTIFIER_LENGTH;

        Group_c *g = get_group_c(g_c, grounumber);

        if (!g) {
            return -1;
        }

        g->invite_called = false;

        if (*data & 1) {
            g->keep_leave = true;
            g->disable_auto_join = true;
        } else {
            g->join_mode = true;
        }


        ++data;
        --length;

        if (*data > sizeof(g->title)) {
            del_groupchat_internal(g_c, grounumber, UNS_NONE);
            return -1;
        }

        g->title_len = *data;
        ++data;
        --length;

        if (length < g->title_len) {
            del_groupchat_internal(g_c, grounumber, UNS_NONE);
            return -1;
        }

        memcpy(g->title, data, g->title_len);
        data += g->title_len;
        length -= g->title_len;

        g->numjoinpeers = lendian_to_host16(*(uint16_t *)data);
        data += sizeof(uint16_t);
        length -= sizeof(uint16_t);

        g->joinpeers = (Group_Join_Peer *)calloc(g->numjoinpeers, sizeof(Group_Join_Peer));

        if (length < g->numjoinpeers * CRYPTO_PUBLIC_KEY_SIZE) {
            del_groupchat_internal(g_c, grounumber, UNS_NONE);
            return -1;
        }

        length -= g->numjoinpeers * CRYPTO_PUBLIC_KEY_SIZE;

        uint64_t t = current_time_monotonic() + 5000;
        size_t j;

        for (j = 0; j < g->numjoinpeers; ++j) {
            memcpy(g->joinpeers[j].real_pk, data, CRYPTO_PUBLIC_KEY_SIZE);
            data += CRYPTO_PUBLIC_KEY_SIZE;
            g->joinpeers[j].next_try_time = t;
        }
    }

    return 0;
}
