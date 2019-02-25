CC=gcc
INSTALL_DIR=/usr/local/bin
# CFLAGS=-Wall -DDEBUG_PRINT_BACKTRACE -fasynchronous-unwind-tables -O0 -fexceptions -finstrument-functions
# LDFLAGS=-L. -Wl,-rpath=. -rdynamic
# CFLAGS=-Wall -pg
CFLAGS=-Wall
#LDFLAGS=-L. -Wl,-rpath=.
LDFLAGS=-L/usr/local/lib -Wl,-rpath=/usr/local/lib
LDLIBS=gh_ctrl.o -lwiringPi -lghpi-sqlite3 -ldl
DEPS=gh_ctrl.h
SRCS=gh_ctrl.c disable_5V.c enable_5V.c read_ina260.c read_TSL2591.c enable_ctrl_board_3V_5V.c disable_ctrl_board_3V_5V.c read_BME680.c read_MAX11201B.c read_VEML6075.c i2c_reset.c read_furnace.c read_SHT31.c poll_stream.c
OBJS=$(subst .c,.o,$(SRCS))
TARGETS=$(filter-out gh_ctrl,$(subst .c,,$(SRCS)))
# PHONY: all clean clean_targets clean_all

all: $(TARGETS)

install:
	cp $(TARGETS) $(INSTALL_DIR)
	cp read_DS18B20.py $(INSTALL_DIR)
clean:
	rm -f $(OBJS)
clean_targets:
	rm -f $(TARGETS)
clean_all: clean clean_targets

disable_5V: disable_5V.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o disable_5V disable_5V.o $(LDLIBS)
enable_5V: enable_5V.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o enable_5V enable_5V.o $(LDLIBS)
read_ina260: read_ina260.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_ina260 read_ina260.o $(LDLIBS) -lm
read_TSL2591: read_TSL2591.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_TSL2591 read_TSL2591.o $(LDLIBS)
enable_ctrl_board_3V_5V: enable_ctrl_board_3V_5V.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o enable_ctrl_board_3V_5V enable_ctrl_board_3V_5V.o $(LDLIBS)
disable_ctrl_board_3V_5V: disable_ctrl_board_3V_5V.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o disable_ctrl_board_3V_5V disable_ctrl_board_3V_5V.o $(LDLIBS)
read_BME680: read_BME680.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_BME680 read_BME680.o $(LDLIBS) -lbme680
read_MAX11201B: read_MAX11201B.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_MAX11201B read_MAX11201B.o $(LDLIBS)
read_VEML6075: read_VEML6075.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_VEML6075 read_VEML6075.o $(LDLIBS)
i2c_reset: i2c_reset.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o i2c_reset i2c_reset.o $(LDLIBS)
read_furnace: read_furnace.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_furnace read_furnace.o $(LDLIBS)
read_SHT31: read_SHT31.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o read_SHT31 read_SHT31.o $(LDLIBS)
poll_stream: poll_stream.o gh_ctrl.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o poll_stream poll_stream.o $(LDLIBS)

disable_5V.c: gh_ctrl.h
read_ina260.c: gh_ctrl.h
read_TSL2591.c: gh_ctrl.h
enable_ctrl_board_3V_5V.c: gh_ctrl.h
disable_ctrl_board_3V_5V.c: gh_ctrl.h
read_BME680.c: gh_ctrl.h
read_MAX11201B.c: gh_ctrl.h
read_VEML6075.c: gh_ctrl.h
i2c_reset.c: gh_ctrl.h
read_furnace.c: gh_ctrl.h
read_SHT31.c: gh_ctrl.h
poll_stream.c: gh_ctrl.h
gh_ctrl.c: gh_ctrl.h
