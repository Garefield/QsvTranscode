WORKDIR = `pwd`

CC = gcc
CXX = g++
AR = ar
LD = g++
WINDRES = windres

INC = -I/home/xstrive/3rd/build/ffmpeg/include -I/opt/intel/mediasdk/include -I/usr/local/include
CFLAGS = -O2
CPPFLAGS = -O2
RESINC =
LIBDIR = -L/home/xstrive/3rd/build/ffmpeg/lib -L/usr/local/lib/dri -L/usr/local/lib -L/opt/intel/mediasdk/lib  -L/home/xstrive/3rd/build/fdk-aac/lib
LIB =
LDFLAGS = -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -lfdk-aac -lboost_thread -lboost_system -lmfx -lva-drm -lva -lva-x11 -lXv -lX11 -lXext -pthread -lm -lz -lstdc++ -ldl


OUT = QSVTransCode

OBJ =  main.o QSVTranscode.o 


all: release

clean: clean_release

before_release:

after_release:

release: before_release out_release after_release

out_release: before_release $(OBJ) $(DEP)
	$(LD) $(LIBDIR) -o $(OUT) $(OBJ)  $(LDFLAGS) $(LIB)


main.o: main.cpp
	$(CXX) $(CPPFLAGS) $(INC) -c main.cpp -o main.o

QSVTranscode.o: QSVTranscode.cpp
	$(CXX) $(CPPFLAGS) $(INC) -c QSVTranscode.cpp -o QSVTranscode.o

clean_release:
	rm -f $(OBJ) $(OUT)

.PHONY: before_release after_release clean_release


