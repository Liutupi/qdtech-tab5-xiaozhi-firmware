LVGL_SRC := $(shell find lvgl/src -name '*.c')
LVGL_OBJ := $(patsubst lvgl/%.c,obj/lvgl/%.o,$(LVGL_SRC))
CFLAGS := -O2 -g0 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
obj/lvgl/%.o: lvgl/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@gcc $(CFLAGS) -c $< -o $@
liblvgl.a: $(LVGL_OBJ)
	ar rcs $@ $^
