/* standard utilities used within the ffi
 *
 * these are mostly-compliant implementations of the standard c++ containers
 * and other utilities, meant to avoid the need for the c++ runtime/standard
 * library; they aren't meant to be general purpose and only implement what
 * we use, they don't provide documented exception safety guarantees and so
 * on either but that's fine since we barely use these at all
 */

#ifndef UTIL_HH
#define UTIL_HH

#include "platform.hh"

#include <type_traits>
#include <cstddef>
#include <cstring>

namespace util {

/* type traits */

namespace detail {
    template<typename T> struct remove_ref { using type = T; };
    template<typename T> struct remove_ref<T &> { using type = T; };
    template<typename T> struct remove_ref<T &&> { using type = T; };
}

template<typename T> using remove_ref_t = typename detail::remove_ref<T>::type;

template<typename T> using remove_sign_t = std::make_unsigned_t<T>;

namespace detail {
    template<bool B, typename T, typename F>
    struct conditional { using type = T; };

    template<typename T, typename F>
    struct conditional<false, T, F> { using type = F; };
}

template<bool B, typename T, typename F>
using conditional_t = typename detail::conditional<B, T, F>::type;

using max_align_t = std::max_align_t;

template<typename T>
struct is_int {
    static constexpr bool value = std::is_integral<T>::value;
};

template<typename T>
struct is_float {
    static constexpr bool value = std::is_floating_point<T>::value;
};

template<typename T>
struct is_arith {
    static constexpr bool value = is_int<T>::value || is_float<T>::value;
};

template<typename T, bool = is_arith<T>::value>
struct is_signed {
    static constexpr bool value = T(-1) < T(0);
};

template<typename T>
struct is_signed<T, false> {
    static constexpr bool value = false;
};

/* move semantics */

template<typename T>
constexpr inline remove_ref_t<T> &&move(T &&v) noexcept {
    return static_cast<remove_ref_t<T> &&>(v);
}

template<typename T>
constexpr inline T &&forward(remove_ref_t<T> &v) noexcept {
    return static_cast<T &&>(v);
}

template<typename T>
constexpr inline T &&forward(remove_ref_t<T> &&v) noexcept {
    return static_cast<T &&>(v);
}

/* assorted utils */

template<typename T, typename U = T>
constexpr inline T exchange(T &v, U &&nv) {
    T ov = move(v);
    v = forward<U>(nv);
    return ov;
}

template<typename T>
inline void swap(T &a, T &b) {
    auto o = move(a);
    a = move(b);
    b = move(o);
}

template<typename T>
inline T min(T a, T b) {
    return (a < b) ? a : b;
}

template<typename T>
inline T max(T a, T b) {
    return (a > b) ? a : b;
}

/* vector */

template<typename T>
struct vector {
    static constexpr size_t MIN_SIZE = 4;

    vector() {}
    ~vector() {
        drop();
    }

    vector(vector const &v) { *this = v; }
    vector(vector &&v) { *this = move(v); }

    vector &operator=(vector const &v) {
        resize(0);
        if (v.size() > capacity()) {
            reserve(v.size());
        }
        for (size_t i = 0; i < v.size(); ++i) {
            push_back(v[i]);
        }
        return *this;
    }

    vector &operator=(vector &&v) {
        swap(v);
        return *this;
    }

    void push_back(T const &v) {
        reserve(p_cap + 1);
        new (&p_buf[p_size++]) T(v);
    }

    void push_back(T &&v) {
        reserve(p_cap + 1);
        new (&p_buf[p_size++]) T(util::move(v));
    }

    void pop_back() {
        p_buf[p_size - 1].~T();
        --p_size;
    }

    template<typename ...A>
    T &emplace_back(A &&...args) {
        reserve(p_cap + 1);
        new (&p_buf[p_size]) T(util::forward<A>(args)...);
        return p_buf[p_size++];
    }

    void reserve(size_t n) {
        if (n <= p_cap) {
            return;
        }
        if (n < MIN_SIZE) {
            n = MIN_SIZE;
        }
        T *np = reinterpret_cast<T *>(new unsigned char[n * sizeof(T)]);
        if (p_cap) {
            for (size_t i = 0; i < p_size; ++i) {
                new (&np[i]) T(util::move(p_buf[i]));
                p_buf[i].~T();
            }
            delete[] reinterpret_cast<unsigned char *>(p_buf);
        }
        p_buf = np;
        p_cap = n;
    }

    void resize(size_t n) {
        for (size_t i = n; i < p_size; ++i) {
            p_buf[i].~T();
        }
        p_size = n;
    }

    void clear() {
        resize(0);
    }

    T &operator[](size_t i) {
        return p_buf[i];
    }

    T const &operator[](size_t i) const {
        return p_buf[i];
    }

    T &front() {
        return p_buf[0];
    }

    T const &front() const {
        return p_buf[0];
    }

    T &back() {
        return p_buf[p_size - 1];
    }

    T const &back() const {
        return p_buf[p_size - 1];
    }

    T *data() {
        return p_buf;
    }

    T const *data() const {
        return p_buf;
    }

    size_t size() const {
        return p_size;
    }

    size_t capacity() const {
        return p_cap;
    }

    bool empty() const {
        return p_size == 0;
    }

    void swap(vector &v) {
        util::swap(p_buf, v.p_buf);
        util::swap(p_size, v.p_size);
        util::swap(p_cap, v.p_cap);
    }

private:
    void drop() {
        resize(0);
        delete[] reinterpret_cast<unsigned char *>(p_buf);
    }

    T *p_buf = nullptr;
    size_t p_size = 0, p_cap = 0;
};

/* hashtable */

template<typename K, typename V, typename HF, typename CF>
struct map {
private:
    static constexpr size_t CHUNK_SIZE = 64;
    static constexpr size_t DEFAULT_SIZE = 1024;

    struct entry {
        K key;
        V data;
    };

    struct bucket {
        entry value;
        bucket *next;
    };

public:
    map(size_t sz = DEFAULT_SIZE): p_size{sz} {
        p_buckets = new bucket *[sz];
        memset(p_buckets, 0, sz * sizeof(bucket *));
    }

    ~map() {
        delete[] p_buckets;
        drop_chunks();
    }

    bool empty() const {
        return p_nelems == 0;
    }

    V &operator[](K const &key) {
        size_t h;
        bucket *b = find_bucket(key, h);
        if (!b) {
            b = add(h);
            b->value.key = key;
        }
        return b->value.data;
    }

    V *find(K const &key) const {
        size_t h;
        bucket *b = find_bucket(key, h);
        if (!b) {
            return nullptr;
        }
        return &b->value.data;
    }

    V &insert(K const &key, V const &value) {
        size_t h;
        bucket *b = find_bucket(key, h);
        if (!b) {
            b = add(h);
            b->value.key = key;
            b->value.data = value;
        }
        return b->value.data;
    }

    void clear() {
        if (!p_nelems) {
            return;
        }
        p_nelems = 0;
        p_unused = nullptr;
        memset(p_buckets, 0, p_size * sizeof(bucket *));
        drop_chunks();
    }

    void swap(map &m) {
        util::swap(p_size, m.p_size);
        util::swap(p_nelems, m.p_nelems);
        util::swap(p_buckets, m.p_buckets);
        util::swap(p_unused, m.p_unused);
        util::swap(p_chunks, m.p_chunks);
    }

    template<typename F>
    void for_each(F &&func) const {
        for (size_t i = 0; i < p_size; ++i) {
            for (bucket *b = p_buckets[i]; b; b = b->next) {
                func(b->value.key, b->value.data);
            }
        }
    }

private:
    bucket *add(size_t hash) {
        if (!p_unused) {
            chunk *nb = new chunk;
            nb->next = p_chunks;
            p_chunks = nb;
            for (size_t i = 0; i < CHUNK_SIZE - 1; ++i) {
                nb->buckets[i].next = &nb->buckets[i + 1];
            }
            nb->buckets[CHUNK_SIZE - 1].next = p_unused;
            p_unused = nb->buckets;
        }
        bucket *b = p_unused;
        p_unused = p_unused->next;
        b->next = p_buckets[hash];
        p_buckets[hash] = b;
        ++p_nelems;
        return b;
    }

    bucket *find_bucket(K const &key, size_t &h) const {
        h = HF{}(key) % p_size;
        for (bucket *b = p_buckets[h]; b; b = b->next) {
            if (CF{}(key, b->value.key)) {
                return b;
            }
        }
        return nullptr;
    }

    void drop_chunks() {
        for (chunk *nc; p_chunks; p_chunks = nc) {
            nc = p_chunks->next;
            delete p_chunks;
        }
    }

    struct chunk {
        bucket buckets[CHUNK_SIZE];
        chunk *next;
    };

    size_t p_size, p_nelems = 0;

    bucket **p_buckets;
    bucket *p_unused = nullptr;
    chunk *p_chunks = nullptr;
};

template<typename T, T offset_basis, T prime>
struct fnv1a {
    T operator()(char const *data) const {
        size_t slen = strlen(data);
        T hash = offset_basis;
        for (size_t i = 0; i < slen; ++i) {
            hash ^= T(data[i]);
            hash *= prime;
        }
        return hash;
    }
};

#if FFI_WORDSIZE == 64
struct str_hash: fnv1a<size_t,
    size_t(14695981039346656037ULL), size_t(1099511628211ULL)
> {};
#elif FFI_WORDSIZE == 32
struct str_hash: fnv1a<size_t, size_t(2166136261U), size_t(16777619U)> {};
#else
#  error Not implemented
#endif

struct str_equal {
    bool operator()(char const *k1, char const *k2) const {
        return !strcmp(k1, k2);
    }
};

template<typename V>
using str_map = map<char const *, V, str_hash, str_equal>;

} /* namespace util */

#endif /* UTIL_HH */