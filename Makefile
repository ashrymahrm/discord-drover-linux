CC ?= gcc
CFLAGS ?= -fPIC -shared -O2 -Wall -Wextra
LDFLAGS ?= -ldl -lpthread

TARGET = libdrover.so
SRCS = libdrover.c drover_packet_blob.c

all: $(TARGET)

drover_packet_blob.c: drover-packet.bin
	xxd -i $< > $@

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install-deps:
	sudo apt-get update && sudo apt-get install -y python3 python3-tk build-essential xxd

clean:
	rm -f $(TARGET) drover_packet_blob.c

.PHONY: all clean install-deps
