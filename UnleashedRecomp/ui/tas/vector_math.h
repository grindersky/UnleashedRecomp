#pragma once

// Vectors and transforms for the TAS HUD and the collision overlay, in single precision like the game.

#include "guest_memory.h"

#include <cmath>
#include <numbers>

namespace Tas
{
    inline constexpr float PI = std::numbers::pi_v<float>;

    struct Vector3
    {
        float x, y, z;

        Vector3 operator+(const Vector3& other) const { return { x + other.x, y + other.y, z + other.z }; }
        Vector3 operator-(const Vector3& other) const { return { x - other.x, y - other.y, z - other.z }; }
        Vector3 operator*(float scale) const { return { x * scale, y * scale, z * scale }; }
    };

    inline float Length(const Vector3& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    inline float Dot(const Vector3& a, const Vector3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vector3 Cross(const Vector3& a, const Vector3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    inline Vector3 Normalised(const Vector3& v)
    {
        float length = Length(v);
        return length > 0.0f ? v * (1.0f / length) : Vector3{ 0.0f, 1.0f, 0.0f };
    }

    inline Vector3 LoadVector(uint32_t address)
    {
        return { LoadFloat(address), LoadFloat(address + 4), LoadFloat(address + 8) };
    }

    // A transform as Havok keeps it: the rotation's columns, then the translation.
    struct Transform
    {
        Vector3 columns[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
        Vector3 translation{};

        static Transform At(uint32_t address)
        {
            Transform transform;
            for (int i = 0; i < 3; i++)
                transform.columns[i] = LoadVector(address + i * 16);

            transform.translation = LoadVector(address + 48);
            return transform;
        }

        // Another transform, in this one's space.
        Transform operator*(const Transform& other) const
        {
            Transform result;
            for (int i = 0; i < 3; i++)
                result.columns[i] = Rotate(other.columns[i]);

            result.translation = (*this)(other.translation);
            return result;
        }

        Vector3 operator()(const Vector3& p) const
        {
            return Rotate(p) + translation;
        }

        Vector3 Rotate(const Vector3& v) const
        {
            return columns[0] * v.x + columns[1] * v.y + columns[2] * v.z;
        }
    };
}
