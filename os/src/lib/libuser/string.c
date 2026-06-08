#include <string.h>
#include <sys/types.h>

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

size_t strlen(const char* s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

char* strcpy(char* dst, const char* src) {
    if (!dst || !src) return dst;
    char* p = dst;
    while ((*p++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char* s1, const char* s2) {
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (!s1[i]) return 0;
    }
    return 0;
}

char* strcat(char* dst, const char* src) {
    if (!dst || !src) return dst;
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strchr(const char* s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char*)s;
        s++;
    }
    return (ch == '\0') ? (char*)s : NULL;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char*)haystack;
        haystack++;
    }
    return NULL;
}

size_t strcspn(const char* s, const char* reject) {
    size_t count = 0;
    while (s[count]) {
        int matched = 0;
        for (const char* r = reject; *r; r++) {
            if (s[count] == *r) { matched = 1; break; }
        }
        if (matched) break;
        count++;
    }
    return count;
}

size_t strspn(const char* s, const char* accept) {
    size_t count = 0;
    while (s[count]) {
        int found = 0;
        for (const char* a = accept; *a; a++) {
            if (s[count] == *a) { found = 1; break; }
        }
        if (!found) break;
        count++;
    }
    return count;
}
