#/######################################################################
# This is the makefile for this test harness.
# Type make with one of the following choices for environments:
#
#      tcp         : You start the receiver and transmitter manually
#      mtcp        : Same as TCP, but uses the mTCP user-mode stack
#
#      For more information, see the function PrintUsage () in harness.c
#
########################################################################

CC         = gcc
CFLAGS     = -g -O3 -Wall -Werror -pthread
SRC        = ./src
ERPC	   = /proj/sequencer/eRPC

all: tcp mtcp udp erpc

clean:
	rm -f *.o NP* np.out

#--- All the stuff needed to compile mTCP ---#
# DPDK library and header 
DPDK_INC_MTCP	= /proj/sequencer/mtcp/dpdk/include
DPDK_LIB_MTCP	= /proj/sequencer/mtcp/dpdk/lib/

# mTCP library and header 
MTCP_FLD    = /proj/sequencer/mtcp/mtcp
MTCP_INC    = -I${MTCP_FLD}/include
MTCP_LIB    = -L${MTCP_FLD}/lib
MTCP_TARGET = ${MTCP_LIB}/libmtcp.a

UTIL_FLD = /proj/sequencer/mtcp/util
UTIL_INC = -I${UTIL_FLD}/include

INC 	= -I./include/ ${UTIL_INC} ${MTCP_INC} -I${UTIL_FLD}/include
MTCP_LIBS	= ${MTCP_LIB}

# CFLAGS for DPDK-related compilation
INC 	+= ${MTCP_INC}
DPDK_MACHINE_FLAGS = $(shell cat /proj/sequencer/mtcp/dpdk/include/cflags.txt)
INC 	+= ${DPDK_MACHINE_FLAGS} -I${DPDK_INC_MTCP} -include $(DPDK_INC_MTCP)/rte_config.h

DPDK_LIB_FLAGS = $(shell cat /proj/sequencer/mtcp/dpdk/lib/ldflags.txt)
MTCP_LIBS 	+= -m64 -g -O3 -pthread -lrt -march=native -export-dynamic ${MTCP_FLD}/lib/libmtcp.a -L../../dpdk/lib -lnuma -lpthread -lrt -ldl ${DPDK_LIB_FLAGS}

ERPC_LIBS	= -lerpc -lpthread -lnuma -ldl

#--- Stuff needed to compile TCATS ---#
tcats_dpdk_dir=/proj/sequencer/seq_theano/sequencer/DPDK
TCATS_INC 	= -I${tcats_dpdk_dir}/include 
TCATS_INC 	+= $(shell cat $${tcats_dpdk_dir}/lib/ldflags.txt) -I${tcats_dpdk_dir}/include/rte_config.h
TCATS_LIBS 		= -lrt -march=native -lnuma -lpthread -ldl
TCATS_LIBS  	+= $(shell cat ${tcats_dpdk_dir}/lib/ldflags.txt)


#--- Compile the binaries ---#

tcp: $(SRC)/tcp.c $(SRC)/harness.c $(SRC)/harness.h 
	$(CC) $(CFLAGS) $(SRC)/harness.c $(SRC)/tcp.c -DTCP -Dwhichproto=\"TCP\" -o NPtcp -I$(SRC)

udp: $(SRC)/udp.c $(SRC)/harness.c $(SRC)/harness.h 
	$(CC) $(CFLAGS) $(SRC)/harness.c $(SRC)/udp.c -DTCP -Dwhichproto=\"UDP\" -o NPudp -I$(SRC)

mtcp: $(SRC)/mtcp.c $(SRC)/harness.c $(SRC)/harness.h 
	$(CC) $(CFLAGS) $(SRC)/harness.c $(SRC)/mtcp.c -DMTCP -Dwhichproto=\"mTCP\" -o NPmtcp -I$(SRC) ${INC} ${MTCP_LIBS} -lmtcp -lnuma -pthread -lrt

erpc: $(SRC)/erpc.cc $(SRC)/harness.c $(SRC)/harness.h
	g++ -g -std=c++11 -o NPerpc $(SRC)/harness.c $(SRC)/erpc.cc -I $(ERPC)/src -I /usr/include/dpdk -L $(ERPC)/build $(ERPC_LIBS) -ldpdk -DDPDK=true -DERPC -Dwhichproto=\"ERPC\"

tcats: $(SRC)/tcats.c $(SRC)/harness.c $(SRC)/harness.h
	$(CC) $(CFLAGS) $(SRC)/harness.c $(SRC)/tcats.c -DTCATS -Dwhichproto=\"TCATS\" -o NPtcats -I$(SRC) $(TCATS_INC) $(TCATS_LIBS)

