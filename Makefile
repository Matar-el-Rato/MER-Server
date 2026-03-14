CC = gcc
CFLAGS = -Wall -I. $(shell mysql_config --cflags)
LDFLAGS = $(shell mysql_config --libs) -lcrypt

# If we were using threads, we would add -lpthread
# LDFLAGS += -lpthread

SRCS = server.c db.c
OBJS = $(SRCS:.c=.o)
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
