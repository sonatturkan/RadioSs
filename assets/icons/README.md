# Uygulama simgesi

- `radios.svg`: RadioS işareti (32 piksel ve üstü): koyu karo, açık renkli "S", camgöbeği yayın yayları
  (tasarımın `#38d4ff` neon vurgusu). Yalnız yol (path) içerir; çizim için yazı tipi gerekmez.
- `radios-small.svg`: 16-24 piksel için sadeleştirilmiş biçim (karo ve kalın "S").
- `radios.ico`: `src/app/radios.rc` ile her RadioS programına gömülen çok boyutlu simge
  (16, 20, 24, 32, 40, 48, 64 piksel 32 bit DIB; 256 piksel PNG). GDK-Win32 pencere ve görev çubuğu
  simgesi olarak programın ilk simge kaynağını kullanır.

Derleme yalnız işlenmiş `radios.ico` dosyasını kullanır. SVG değişince simgeyi MSYS2 UCRT64 kabuğunda
yeniden üretin:

```sh
for s in 16 20 24; do rsvg-convert -w $s -h $s -o small-$s.png assets/icons/radios-small.svg; done
for s in 32 40 48 64 256; do rsvg-convert -w $s -h $s -o full-$s.png assets/icons/radios.svg; done
python tools/make_ico.py assets/icons/radios.ico small-16.png small-20.png small-24.png \
    full-32.png full-40.png full-48.png full-64.png full-256.png
```

`tools/make_ico.py` yalnız standart kütüphaneyi kullanır ve aynı PNG'lerden her zaman aynı baytları üretir.
