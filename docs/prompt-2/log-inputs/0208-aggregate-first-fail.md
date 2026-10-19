## Gerçek aggregate20 ilk yürütme FAIL ve kesin mixer sınırı teşhisi

Architecture ilk aggregate test sonucu `/genuine-main-playing` deck3 için `current==FALSE` failure'ını inceledi. **Kesin üretim business bug:** mapping loop stereo sample channel sayısı `RADIOS_AUDIO_CHANNELS` (2) ile yürütülüyordu; gerçek mixer routing **32 strip**, `RADIOS_AUDIO_MIXER_CHANNELS` veya `G_N_ELEMENTS(raw.mixer.channels)` kadar gözlenmelidir. İki strip dışındaki gerçek MAIN deck'ler yanlış dışlanıyordu.

Architecture test fixture reset loop'unda da aynı makro hatası vardı; kendi test kaynak sahipliği içinde `G_N_ELEMENTS` 32'ye düzeltti. Böylece unmapped0 case'inde kalan30 zero-strip sahte mapping verilmez. Üretim aggregate loop fix sahibi root; architecture bu kaynağı değiştirmedi.

İlk FAIL açık kaydedildi; önceki component8 veya strict compile PASS, bu gerçek aggregate business failure'ını kapsamıyordu. Root üretim mapping sınırını düzelttikten sonra yeniden strict build ve bütün20case run beklenir. Güncel bu suite PASS değildir; fixture fix tek başına üretim fix kanıtı sayılmaz.

Tamamlanan: kesin kök neden; test fixture düzeltmesi; root'a üretim fix devri. Bekleyen: gerçek aggregate fix +20case retest; online fixture; layout6 ve final main/regression kabul.
