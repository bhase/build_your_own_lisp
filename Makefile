
targets := prompt parsing eval error_handling s_expression q_expression

all: $(targets)

define TARGET_template =
$(1): $(addsuffix .c,$1) mpc.c
	gcc -Wall -std=c99 $$^ -lm -lreadline -o $$@
endef

$(foreach prog,$(targets),$(eval $(call TARGET_template,$(prog))))

clean:
	rm $(targets)
