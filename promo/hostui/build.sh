set -e
T=/home/user/qdtech-tab5-xiaozhi-firmware/main/boards/qdtech/tab5
CF="-O1 -I shim -I $T -I . -I lvgl -I lvgl/src -I xiaozhi-fonts/include -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -w"
c() { o=obj/app/$(basename $1).o; if [ ! -f $o ] || [ $1 -nt $o ]; then echo "cc $1"; $2 $CF -c $1 -o $o; fi; }
for f in gen/qd_font_lxgw_28.c gen/qd_font_lxgw_36.c; do c $f gcc & done
for f in qd_font_cjk_28.c qd_font_clock_72.c nabo_assets.c nabo_portrait_matte.c nabo_sd_static.c; do c $T/$f gcc & done; wait
for f in tab5_native_apps.cc tab5_podcast_page.cc tab5_icu_page.cc tab5_ir_remote_page.cc tab5_sd_scene.cc tab5_daily_content.cc tab5_clinical_pearls.cc tab5_lrc.cc icu_calculators.cc; do c $T/$f "g++ -std=gnu++23" & done
c src/host_cbin.c gcc
c src/harness.cc "g++ -std=gnu++23 -fno-access-control"
c src/stubs.cc "g++ -std=gnu++23"
wait
gcc -c gen/portrait.S -o obj/app/portrait.o; gcc -c gen/sdstatic.S -o obj/app/sdstatic.o
g++ -o harness obj/app/*.o liblvgl.a -lm
