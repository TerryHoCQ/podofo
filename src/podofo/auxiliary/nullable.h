/**
 * SPDX-FileCopyrightText: (C) 2022 Francesco Pretto <ceztko@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef AUX_NULLABLE_H
#define AUX_NULLABLE_H
#pragma once

#include <cstddef>
#include <stdexcept>
#include <type_traits>

namespace PoDoFo
{
    class bad_nullable_access : public std::runtime_error
    {
    public:
        bad_nullable_access()
            : std::runtime_error("nullable object doesn't have a value") { }
    };

    /**
     * Alternative to std::optional that supports reference (but not pointer) types
     */
    template <typename T, typename = std::enable_if_t<!std::is_pointer_v<T>>>
    class nullable final
    {
    public:
        nullable()
            : m_value{ }, m_hasValue(false) { }

        nullable(T value)
            : m_value(std::move(value)), m_hasValue(true) { }

        nullable(std::nullptr_t)
            : m_value{ }, m_hasValue(false) { }

        nullable(const nullable& value) = default;

        nullable& operator=(const nullable& value) = default;

        nullable& operator=(T value)
        {
            m_hasValue = true;
            m_value = std::move(value);
            return *this;
        }

        nullable& operator=(std::nullptr_t)
        {
            m_hasValue = false;
            m_value = { };
            return *this;
        }

        const T& value() const
        {
            if (!m_hasValue)
                throw bad_nullable_access();

            return m_value;
        }

        T& value()
        {
            if (!m_hasValue)
                throw bad_nullable_access();

            return m_value;
        }

        bool has_value() const { return m_hasValue; }
        const T* operator->() const { return &m_value; }
        T* operator->() { return &m_value; }
        const T& operator*() const { return m_value; }
        T& operator*() { return m_value; }

    public:
        template <typename T2>
        friend bool operator==(const nullable<T2>& lhs, const nullable<T2>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2>& lhs, const nullable<T2>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<std::decay_t<T2>>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<std::decay_t<T2>>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2&>& lhs, const nullable<std::decay_t<T2>>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2&>& lhs, const nullable<std::decay_t<T2>>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2>& lhs, const T2& rhs);

        template <typename T2>
        friend bool operator==(const T2& lhs, const nullable<T2>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2>& lhs, std::nullptr_t);

        template <typename T2>
        friend bool operator!=(const nullable<T2>& lhs, const T2& rhs);

        template <typename T2>
        friend bool operator!=(const T2& lhs, const nullable<T2>& rhs);

        template <typename T2>
        friend bool operator==(std::nullptr_t, const nullable<T2>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2>& lhs, std::nullptr_t);

        template <typename T2>
        friend bool operator!=(std::nullptr_t, const nullable<T2>& rhs);

    private:
        T m_value;
        bool m_hasValue;
    };

    // Template specialization for references
    template <typename T>
    class nullable<T&> final
    {
    public:
        nullable()
            : m_value{ }, m_hasValue(false){ }

        nullable(T& value)
            : m_value(&value), m_hasValue(true) { }

        nullable(std::nullptr_t)
            : m_value{ }, m_hasValue(false) { }

        // Allow nullable<const T&>::nullable(const nullable<T&>&)
        template <typename T2, std::enable_if_t<std::is_convertible_v<std::add_pointer_t<std::remove_reference_t<T2>>,
            std::add_pointer_t<std::remove_reference_t<T>>>, int> = 0>
        nullable(const nullable<T2&>& value)
            : m_value(reinterpret_cast<const nullable&>(value).m_value), m_hasValue(reinterpret_cast<const nullable&>(value).m_hasValue) { }

        nullable(const nullable& value) = default;

        nullable& operator=(const nullable& value) = default;

        const T& value() const
        {
            if (!m_hasValue)
                throw bad_nullable_access();

            return *m_value;
        }

        T& value()
        {
            if (!m_hasValue)
                throw bad_nullable_access();

            return *m_value;
        }

        bool has_value() const { return m_hasValue; }
        const T* operator->() const { return m_value; }
        T* operator->() { return m_value; }
        const T& operator*() const { return *m_value; }
        T& operator*() { return *m_value; }

    public:
        template <typename T2>
        friend bool operator==(const nullable<std::decay_t<T2>>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<std::decay_t<T2>>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2&>& lhs, const nullable<std::decay_t<T2>>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2&>& lhs, const nullable<std::decay_t<T2>>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2&>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2&>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2&>& lhs, const std::decay_t<T2>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2&>& lhs, const std::decay_t<T2>& rhs);

        template <typename T2>
        friend bool operator==(const std::decay_t<T2>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator!=(const std::decay_t<T2>& lhs, const nullable<T2&>& rhs);

        template <typename T2>
        friend bool operator==(const nullable<T2>& lhs, std::nullptr_t);

        template <typename T2>
        friend bool operator==(std::nullptr_t, const nullable<T2>& rhs);

        template <typename T2>
        friend bool operator!=(const nullable<T2>& lhs, std::nullptr_t);

        template <typename T2>
        friend bool operator!=(std::nullptr_t, const nullable<T2>& rhs);

    private:
        T* m_value;
        bool m_hasValue;
    };

    template <typename T2>
    bool operator==(const nullable<T2>& lhs, const nullable<T2>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return false;

        if (lhs.m_hasValue)
            return lhs.m_value == rhs.m_value;
        else
            return true;
    }

    template <typename T2>
    bool operator!=(const nullable<T2>& lhs, const nullable<T2>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return true;

        if (lhs.m_hasValue)
            return lhs.m_value != rhs.m_value;
        else
            return false;
    }

    template <typename T2>
    bool operator==(const nullable<std::decay_t<T2>>& lhs, const nullable<T2&>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return false;

        if (lhs.m_hasValue)
            return lhs.m_value == *rhs.m_value;
        else
            return true;
    }

    template <typename T2>
    bool operator!=(const nullable<std::decay_t<T2>>& lhs, const nullable<T2&>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return true;

        if (lhs.m_hasValue)
            return lhs.m_value != *rhs.m_value;
        else
            return false;
    }

    template <typename T2>
    bool operator==(const nullable<T2&>& lhs, const nullable<std::decay_t<T2>>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return false;

        if (lhs.m_hasValue)
            return *lhs.m_value == rhs.m_value;
        else
            return true;
    }

    template <typename T2>
    bool operator!=(const nullable<T2&>& lhs, const nullable<std::decay_t<T2>>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return true;

        if (lhs.m_hasValue)
            return *lhs.m_value != rhs.m_value;
        else
            return false;
    }

    template <typename T2>
    bool operator==(const nullable<T2&>& lhs, const nullable<T2&>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return false;

        if (lhs.m_hasValue)
            return *lhs.m_value == *rhs.m_value;
        else
            return true;
    }

    template <typename T2>
    bool operator!=(const nullable<T2&>& lhs, const nullable<T2&>& rhs)
    {
        if (lhs.m_hasValue != rhs.m_hasValue)
            return true;

        if (lhs.m_hasValue)
            return *lhs.m_value != *rhs.m_value;
        else
            return false;
    }

    template <typename T2>
    bool operator==(const nullable<T2&>& lhs, const std::decay_t<T2>& rhs)
    {
        if (!lhs.m_hasValue)
            return false;

        return *lhs.m_value == rhs;
    }

    template <typename T2>
    bool operator!=(const nullable<T2&>& lhs, const std::decay_t<T2>& rhs)
    {
        if (!lhs.m_hasValue)
            return true;

        return *lhs.m_value != rhs;
    }

    template <typename T2>
    bool operator==(const std::decay_t<T2>& lhs, const nullable<T2&>& rhs)
    {
        if (!rhs.m_hasValue)
            return false;

        return lhs == *rhs.m_value;
    }

    template <typename T2>
    bool operator!=(const std::decay_t<T2>& lhs, const nullable<T2&>& rhs)
    {
        if (!rhs.m_hasValue)
            return true;

        return lhs != *rhs.m_value;
    }

    template <typename T2>
    bool operator==(const nullable<T2>& lhs, const T2& rhs)
    {
        if (!lhs.m_hasValue)
            return false;

        return lhs.m_value == rhs;
    }

    template <typename T2>
    bool operator!=(const nullable<T2>& lhs, const T2& rhs)
    {
        if (!lhs.m_hasValue)
            return true;

        return lhs.m_value != rhs;
    }

    template <typename T2>
    bool operator==(const T2& lhs, const nullable<T2>& rhs)
    {
        if (!rhs.m_hasValue)
            return false;

        return lhs == rhs.m_value;
    }

    template <typename T2>
    bool operator!=(const T2& lhs, const nullable<T2>& rhs)
    {
        if (!rhs.m_hasValue)
            return true;

        return lhs != rhs.m_value;
    }

    template <typename T2>
    bool operator==(const nullable<T2>& lhs, std::nullptr_t)
    {
        return !lhs.m_hasValue;
    }

    template <typename T2>
    bool operator!=(const nullable<T2>& lhs, std::nullptr_t)
    {
        return lhs.m_hasValue;
    }

    template <typename T2>
    bool operator==(std::nullptr_t, const nullable<T2>& rhs)
    {
        return !rhs.m_hasValue;
    }

    template <typename T2>
    bool operator!=(std::nullptr_t, const nullable<T2>& rhs)
    {
        return rhs.m_hasValue;
    }
}

#endif // AUX_NULLABLE_H
