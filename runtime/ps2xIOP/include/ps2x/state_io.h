#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps2x::state
{
    struct Error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    constexpr uint32_t fourcc(const char (&text)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(text[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8u) |
               (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16u) | (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24u);
    }

    class Writer
    {
    public:
        static constexpr bool kLoading = false;

        void raw(const void *data, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            m_data.insert(m_data.end(), bytes, bytes + size);
        }

        void section(uint32_t tag)
        {
            raw(&tag, sizeof(tag));
        }

        [[nodiscard]] const std::vector<uint8_t> &data() const noexcept { return m_data; }
        [[nodiscard]] std::vector<uint8_t> &data() noexcept { return m_data; }

    private:
        std::vector<uint8_t> m_data;
    };

    class Reader
    {
    public:
        static constexpr bool kLoading = true;

        Reader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

        void raw(void *data, size_t size)
        {
            if (size > m_size - m_position)
                throw Error("state data ends early");
            std::memcpy(data, m_data + m_position, size);
            m_position += size;
        }

        void section(uint32_t tag)
        {
            uint32_t actual = 0u;
            raw(&actual, sizeof(actual));
            if (actual != tag)
            {
                char expected[5] = {static_cast<char>(tag), static_cast<char>(tag >> 8u), static_cast<char>(tag >> 16u),
                                    static_cast<char>(tag >> 24u), 0};
                throw Error(std::string("state section mismatch, expected ") + expected);
            }
        }

        [[nodiscard]] size_t remaining() const noexcept { return m_size - m_position; }

        [[nodiscard]] bool nextSectionIs(uint32_t tag) const noexcept
        {
            uint32_t actual = 0u;
            if (remaining() < sizeof(actual))
                return false;
            std::memcpy(&actual, m_data + m_position, sizeof(actual));
            return actual == tag;
        }

    private:
        const uint8_t *m_data = nullptr;
        size_t m_size = 0u;
        size_t m_position = 0u;
    };

    template <typename Ar, typename T>
    void field(Ar &ar, T &value);

    template <typename Ar>
    uint64_t count(Ar &ar, size_t size)
    {
        uint64_t n = size;
        ar.raw(&n, sizeof(n));
        if constexpr (Ar::kLoading)
        {
            if (n > (uint64_t{1} << 32u))
                throw Error("state container size is implausible");
        }
        return n;
    }

    template <typename Ar>
    void field(Ar &ar, std::string &value)
    {
        const uint64_t n = count(ar, value.size());
        if constexpr (Ar::kLoading)
            value.resize(static_cast<size_t>(n));
        if (n != 0u)
            ar.raw(value.data(), static_cast<size_t>(n));
    }

    template <typename Ar, typename T, typename A>
    void field(Ar &ar, std::vector<T, A> &value)
    {
        const uint64_t n = count(ar, value.size());
        if constexpr (Ar::kLoading)
            value.resize(static_cast<size_t>(n));
        if constexpr (std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
        {
            if (n != 0u)
                ar.raw(value.data(), static_cast<size_t>(n) * sizeof(T));
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            for (size_t i = 0; i < n; ++i)
            {
                uint8_t bit = value[i] ? 1u : 0u;
                ar.raw(&bit, 1u);
                if constexpr (Ar::kLoading)
                    value[i] = bit != 0u;
            }
        }
        else
        {
            for (auto &item : value)
                field(ar, item);
        }
    }

    template <typename Ar, typename T, typename A>
    void field(Ar &ar, std::deque<T, A> &value)
    {
        const uint64_t n = count(ar, value.size());
        if constexpr (Ar::kLoading)
            value.resize(static_cast<size_t>(n));
        for (auto &item : value)
            field(ar, item);
    }

    template <typename Ar, typename T, size_t N>
    void field(Ar &ar, std::array<T, N> &value)
    {
        if constexpr (std::is_trivially_copyable_v<T>)
            ar.raw(value.data(), sizeof(T) * N);
        else
            for (auto &item : value)
                field(ar, item);
    }

    template <typename Ar, typename T>
    void field(Ar &ar, std::optional<T> &value)
    {
        uint8_t present = value.has_value() ? 1u : 0u;
        ar.raw(&present, 1u);
        if constexpr (Ar::kLoading)
        {
            if (present)
                value.emplace();
            else
                value.reset();
        }
        if (value)
            field(ar, *value);
    }

    template <typename Ar, typename K, typename V>
    void field(Ar &ar, std::pair<K, V> &value)
    {
        field(ar, value.first);
        field(ar, value.second);
    }

    template <typename Ar, typename Map>
    void mapField(Ar &ar, Map &value)
    {
        const uint64_t n = count(ar, value.size());
        if constexpr (Ar::kLoading)
        {
            value.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                typename Map::key_type key{};
                field(ar, key);
                typename Map::mapped_type mapped{};
                field(ar, mapped);
                value.emplace(std::move(key), std::move(mapped));
            }
        }
        else
        {
            for (auto &[key, mapped] : value)
            {
                auto keyCopy = key;
                field(ar, keyCopy);
                field(ar, mapped);
            }
        }
    }

    template <typename Ar, typename K, typename V, typename C, typename A>
    void field(Ar &ar, std::map<K, V, C, A> &value) { mapField(ar, value); }

    template <typename Ar, typename K, typename V, typename C, typename A>
    void field(Ar &ar, std::multimap<K, V, C, A> &value) { mapField(ar, value); }

    template <typename Ar, typename K, typename V, typename H, typename E, typename A>
    void field(Ar &ar, std::unordered_map<K, V, H, E, A> &value) { mapField(ar, value); }

    template <typename Ar, typename Set>
    void setField(Ar &ar, Set &value)
    {
        const uint64_t n = count(ar, value.size());
        if constexpr (Ar::kLoading)
        {
            value.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                typename Set::key_type key{};
                field(ar, key);
                value.insert(std::move(key));
            }
        }
        else
        {
            for (const auto &key : value)
            {
                auto keyCopy = key;
                field(ar, keyCopy);
            }
        }
    }

    template <typename Ar, typename K, typename C, typename A>
    void field(Ar &ar, std::set<K, C, A> &value) { setField(ar, value); }

    template <typename Ar, typename K, typename H, typename E, typename A>
    void field(Ar &ar, std::unordered_set<K, H, E, A> &value) { setField(ar, value); }

    template <typename Ar, typename T>
    void field(Ar &ar, T &value)
    {
        if constexpr (requires { value.serialize(ar); })
            value.serialize(ar);
        else if constexpr (requires { serialize(ar, value); })
            serialize(ar, value);
        else
        {
            static_assert(std::is_trivially_copyable_v<T>, "state field needs serialize()");
            ar.raw(&value, sizeof(T));
        }
    }

    template <typename Ar, typename... T>
    void fields(Ar &ar, T &...values)
    {
        (field(ar, values), ...);
    }
}
