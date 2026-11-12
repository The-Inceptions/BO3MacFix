#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MoreMapMemory: 2 GB more address space for the files the game loads when its own 2 GB are full, as All-around
// Enhancement 4's Windows plugin gives it (see pmem_spill.c)
void pmem_spill_init(uint64_t game_base_address, int enabled);
// how much of the game's 2 GB for loaded files is in use, and how much of the 2 GB more (0 while they aren't needed)
size_t pmem_spill_used(size_t *spilled);
