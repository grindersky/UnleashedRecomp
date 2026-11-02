#pragma once

// Reading the game's memory for the TAS HUD and the collision overlay. Guest addresses, big-endian; nothing here writes.

#include <kernel/function.h>
#include <kernel/memory.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace Tas
{
    // Everything but the first page of guest memory can be read.
    inline bool IsReadable(uint32_t address)
    {
        return address >= 0x1000 && address < 0xFFFF0000;
    }

    inline bool IsInImage(uint32_t address)
    {
        return address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    }

    inline uint32_t LoadU32(uint32_t address)
    {
        uint8_t* base = g_memory.base;
        return IsReadable(address) ? PPC_LOAD_U32(address) : 0;
    }

    inline uint16_t LoadU16(uint32_t address)
    {
        uint8_t* base = g_memory.base;
        return IsReadable(address) ? PPC_LOAD_U16(address) : 0;
    }

    inline uint8_t LoadU8(uint32_t address)
    {
        uint8_t* base = g_memory.base;
        return IsReadable(address) ? PPC_LOAD_U8(address) : 0;
    }

    inline float LoadFloat(uint32_t address)
    {
        uint32_t value = LoadU32(address);
        float result;
        memcpy(&result, &value, sizeof(result));
        return result;
    }

    // A CSharedString's text.
    inline std::string GetSharedString(uint32_t address)
    {
        uint32_t text = LoadU32(address);
        if (!IsReadable(text))
            return {};

        auto chars = reinterpret_cast<const char*>(g_memory.Translate(text));
        return std::string(chars, strnlen(chars, 64));
    }

    // Looks up an std::map node like std::map::find, without inserting (the game's lookups insert missing keys).
    // MSVC's tree: the head node at +4 of the map, whose parent (+4) is the root; nodes have their left child at +0,
    // right child at +8, key at +12 and value at +16, then the colour and a byte set in the head: at +25 after an
    // 8-byte value (like a shared_ptr), at +21 after a 2-byte one. Returns the node, or 0.
    template<typename Less, typename Equal>
    uint32_t FindMapNode(uint32_t map, uint32_t headFlagOffset, Less less, Equal equal)
    {
        uint32_t head = LoadU32(map + 4);
        if (head == 0)
            return 0;

        uint32_t found = head;
        uint32_t node = LoadU32(head + 4);
        for (int depth = 0; depth < 64 && node != 0 && LoadU8(node + headFlagOffset) == 0; depth++)
        {
            if (less(node + 12))
            {
                node = LoadU32(node + 8);
            }
            else
            {
                found = node;
                node = LoadU32(node);
            }
        }

        return found != head && equal(found + 12) ? found : 0;
    }

    inline uint32_t FindMapNode(uint32_t map, uint16_t key, uint32_t headFlagOffset = 25)
    {
        return FindMapNode(map, headFlagOffset, [&](uint32_t k) { return LoadU16(k) < key; }, [&](uint32_t k) { return LoadU16(k) == key; });
    }

    inline uint32_t FindMapNode(uint32_t map, const std::string& key, uint32_t headFlagOffset = 25)
    {
        return FindMapNode(map, headFlagOffset, [&](uint32_t k) { return GetSharedString(k) < key; },
            [&](uint32_t k) { return GetSharedString(k) == key; });
    }

    // Calls visit(index, mangled name) for a vtable's classes in the game's RTTI, the class itself first, until it
    // returns true: the vtable is preceded by its complete object locator, whose class hierarchy (+16) lists its classes
    // (+12, how many at +8), each with its type descriptor first, whose mangled name is at +8.
    template<typename Visit>
    void ForEachClassName(uint32_t vftable, const Visit& visit)
    {
        if (!IsInImage(vftable))
            return;

        uint32_t hierarchy = LoadU32(LoadU32(vftable - 4) + 16);
        uint32_t count = LoadU32(hierarchy + 8);
        uint32_t classes = LoadU32(hierarchy + 12);
        if (count >= 64)
            return;

        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t descriptor = LoadU32(LoadU32(classes + i * 4));
            if (!IsInImage(descriptor))
                break;

            auto text = reinterpret_cast<const char*>(g_memory.Translate(descriptor + 8));
            if (visit(i, std::string_view(text, strnlen(text, 128))))
                break;
        }
    }
}
