CC ?= gcc

# mbedTLS flags (use pkg-config if available, otherwise try brew paths)
MBEDTLS_CFLAGS := $(shell pkg-config --cflags mbedtls 2>/dev/null)
MBEDTLS_LIBS := $(shell pkg-config --libs mbedtls 2>/dev/null)

# Fallback: detect via brew on macOS
ifeq ($(MBEDTLS_CFLAGS),)
    BREW_MBEDTLS := $(shell brew --prefix mbedtls 2>/dev/null)
    ifneq ($(BREW_MBEDTLS),)
        MBEDTLS_CFLAGS := -I$(BREW_MBEDTLS)/include
        MBEDTLS_LIBS := -L$(BREW_MBEDTLS)/lib -lmbedtls -lmbedcrypto -lmbedx509
    else
        MBEDTLS_LIBS := -lmbedtls -lmbedcrypto -lmbedx509
    endif
endif

CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -I. -Iquickjs \
         -D_GNU_SOURCE -DCONFIG_VERSION=\"2024-02-14\" -DCONFIG_BIGNUM \
         $(MBEDTLS_CFLAGS)
LDFLAGS := -lcurl -lz -lpthread -lm -ldl $(MBEDTLS_LIBS)

# macOS: no -ldl needed
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS := $(filter-out -ldl,$(LDFLAGS))
endif

QUICKJS_SRCS = quickjs/quickjs.c quickjs/dtoa.c quickjs/libregexp.c quickjs/libunicode.c
POLYFILL_SRCS = polyfill/polyfill.c polyfill/console.c polyfill/fetch.c \
                polyfill/timer.c polyfill/crypto.c polyfill/buffer.c \
                polyfill/encoding.c polyfill/url.c polyfill/zlib.c
MAIN_SRCS = main.c

SRCS = $(MAIN_SRCS) $(QUICKJS_SRCS) $(POLYFILL_SRCS)
OBJS = $(SRCS:.c=.o)

TARGET = cqjs

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
