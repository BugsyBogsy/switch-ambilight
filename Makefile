.PHONY: setup test build package
setup:
	bash scripts/setup.sh
test:
	bash scripts/test.sh
build:
	bash scripts/build.sh
package:
	bash scripts/package.sh
