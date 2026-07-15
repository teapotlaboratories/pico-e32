# Per-board make settings for the M5Stack Timer Camera F (see ../../Makefile).
#
# Its USB-serial bridge (FTDI, 0403:6001) cannot sustain idf.py's default 460800 baud: the
# chip syncs at 115200 and then dies on the baud switch with "No serial data received".
BAUD := 115200
