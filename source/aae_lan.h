// All-around Enhancement 4's games on the local network, joined as its Windows plugin makes them work (AAELanJoin).
// See aae_lan.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void aae_lan_init(uint64_t game_base_address, int enabled);

// The plugin's query and reply, on UDP port 27018: 24 bytes each, "AAELAN35", a number the asker picks (u32), the
// kind (u32: 1 a query, 2 a reply) and in a reply the host's Steam ID (u64), all little-endian.
#define AAE_LAN_PORT 27018
#define AAE_LAN_PACKET 24

// A Steam ID of a person: an individual account (type 1) in the public universe (1) with an account number.
bool aae_lan_is_individual(uint64_t id);
// An IPv4 address (its first number in the high byte) of a local network: 10/8, 172.16/12, 192.168/16, 127/8 or
// 169.254/16.
bool aae_lan_is_private(uint32_t address);

// A query as the host takes one: exactly 24 bytes, the magic and the kind 1. Gives the asker's number.
bool aae_lan_read_query(const uint8_t *packet, size_t length, uint32_t *number);
// Whether the host answers a query: it came from a local network, the game is in LAN (1) or online (2) mode and the
// host's Steam ID is a person's.
bool aae_lan_answers(uint32_t from, int network_mode, uint64_t id);
// The host's reply: the asker's number and the host's Steam ID.
void aae_lan_reply(uint8_t packet[AAE_LAN_PACKET], uint32_t number, uint64_t id);
