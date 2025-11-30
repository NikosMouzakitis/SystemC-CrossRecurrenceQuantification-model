all:
	g++ -std=c++17 main.cpp -lsystemc -lm -o crqa-model \
    -I/home/x/implementations/systemc-crqa/systemc/install/include \
    -L/home/x/implementations/systemc-crqa/systemc/install/lib

clean:
	rm crqa-model
