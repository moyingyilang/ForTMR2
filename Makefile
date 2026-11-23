# ForTMR2 公开构建入口
#
#   make        lib + demo（纯 C，不依赖 VFS）
#   make lib    资源源抽象库
#   make demo   抽象层演示
#   make test   构建并跑 demo
#   make clean
CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Ilib/include
LDFLAGS ?= -lz
LIB  := lib/libefml4a.a
DEMO := bin/demo_abstract

all: lib demo
lib:
	$(MAKE) -C lib
demo: $(DEMO)
$(DEMO): lib/examples/demo_abstract.c $(LIB) | bin
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS)
bin:
	@mkdir -p bin
test: all
	./$(DEMO) .
clean:
	-$(MAKE) -C lib clean
	rm -rf bin
.PHONY: all lib demo test clean
