CXX = clang++
CXXFLAGS = -std=c++17 -O0  # adapt to the linux env.
interpreter: interpreter.cc
clean:
	rm -f ./interpreter

benchmark:
	python3 ./benchmark.py
