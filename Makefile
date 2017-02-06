PYTHON ?= python

all: lint

lint: cpplint

cpplint.py:
	curl -o cpplint.py "https://raw.githubusercontent.com/nodejs/node/master/tools/cpplint.py"

cpplint: cpplint.py
	python cpplint.py **/*.cc **/*.h

.PHONY: all cpplint lint r
