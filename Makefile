.PHONY: all
all:
	gcc -Wno-int-conversion -o code main.c buddy.c
