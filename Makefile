
targets := 04_prompt 06_parsing 07_eval 08_error_handling 09_s_expression
targets += 10_q_expression 11_variables 12_functions 13_conditionals

all: $(targets)

define TARGET_template =
$(1): $(addsuffix .c,$1) mpc.c
	gcc -Wall -ggdb -std=c99 $$^ -lm -lreadline -o $$@
endef

$(foreach prog,$(targets),$(eval $(call TARGET_template,$(prog))))

clean:
	rm -f $(targets)
