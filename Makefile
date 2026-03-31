CC      = cc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -Isrc
LDFLAGS = -lm
SRC     = src/main.c src/encoder.c src/decoder.c src/edge_analysis.c \
          src/prediction.c src/transform.c src/bitstream.c src/frame.c \
          src/metrics.c
OBJ     = $(SRC:.c=.o)
BIN     = bhevc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c src/bhevc.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
