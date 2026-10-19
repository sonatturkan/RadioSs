# Tez ve ek kullanıcı isteği eşlemesi

Tezdeki kanıt ayrımı korunur: **D** kullanıcı bildirimi, **K** üretici düzeltmesi, **Ö** öneri, **B** belirsiz/eksik doğrulama. Rekordbox doğrudan okuma isteği teze sonradan eklenen kullanıcı gereksinimidir.

| Örnek | Tür | SDBASE karşılığı | Kabul sınırı |
|---|---|---|---|
| LibreTime #701 seçili alanlarda toplu düzenleme | D | `bulk.py`: önizleme, dokunma/set/ekle/çıkar/temizle, revizyonlu uygulama ve koşullu geri alma | 400 parçada yalnız kategori değişimi kontrol edilir; diğer alanlar korunur. |
| LibreTime #1129 zorunlu alan | D | `metadata.py`: türe/aşamaya göre alan tanımı ve eksik listesi | Etkinleştirme uygunluğu eksik alanı bildirir. |
| Rivendell #398 sanatçı/katkı | D | `people`, `person_memberships`, `contributions`, rotasyon açıklaması | Solo/grup/konuk tekrar ayrımı. |
| Rivendell #795 dosya değişiminde metadata | D | Alan bazlı kaynak politikaları ve inceleme önerileri | Onaylı editoryal bilgi sessizce ezilmez. |
| Rivendell #158/#540 görünüm tercihi | D | `settings.py`: kullanıcıya ve istasyona bağlı sürümlü sütun/filtre profili | Operatör arayüzü ve sütun genişliği henüz yoktur. |
| PlayoutONE kimlik/kategori eşitleme | K | UUID, kaynak kapsamlı dış kimlik, istasyon ilişkisi, tombstone | Geçmişte düzeltilmiş hata güncel ürün kusuru sayılmaz. |
| mAirList kilit/sessizlik örneği | B | Kısa işlemler, yerel yayın önbelleği, ayrı motor durumu | Kök nedenin SQLite olduğu iddia edilmez. |
| RadioBOSS API keşif/yedek | D | HTTP yetenek bildirimi, yedek/geri yükleme | Üreticinin kendi API uyumluluğu iddia edilmez. |
| Rekordbox XML/kütüphane | Ek kullanıcı gereksinimi | XML 1.0.0 için salt okunur adaptör | Yerel/USB/OneLibrary şeması doğrulanmadan desteklenmiş sayılmaz. |

Veritabanı kapasitesi, uygulama mantığı, arayüz, medya taşıması ve ses donanımı farklı kabul konularıdır. Bir üründeki kullanıcı sorunu kendi başına o ürünün DBMS motor kusurunu kanıtlamaz.
