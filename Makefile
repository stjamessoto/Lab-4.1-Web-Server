# Lab 4.1 - build everything with `make`, binaries land in ./bin
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pthread

PROGRAMS := echo_server echo_client multi_threaded_server thread_pool_server http_server
BINARIES := $(addprefix bin/,$(PROGRAMS))

SUBMISSION := Lab4.1_submission.zip

.PHONY: all clean zip report

all: $(BINARIES)

bin/%: src/%.cpp src/common.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf bin $(SUBMISSION)
	rm -f report/*.aux report/*.log report/*.out report/*.toc

# Builds report/report.pdf. Run twice so the table of contents is correct.
# Screenshots are picked up from screenshots/ by file name.
report:
	cd report && pdflatex -interaction=nonstopmode -halt-on-error report.tex >/dev/null \
	          && pdflatex -interaction=nonstopmode -halt-on-error report.tex >/dev/null
	@echo "Created report/report.pdf"
	@grep 'MISSING-SHOT' report/report.log | sed 's/MISSING-SHOT /  missing: screenshots\//; s/$$/.png/' || true
	@echo "Screenshots still missing: $$(grep -c 'MISSING-SHOT' report/report.log)"

# Bundles what the lab asks you to hand in (rebuilds the report first).
zip: report
	rm -f $(SUBMISSION)
	zip -r $(SUBMISSION) src www scripts screenshots Makefile README.md report/report.tex report/report.pdf -x 'screenshots/.gitkeep'
	@echo "Created $(SUBMISSION)"
