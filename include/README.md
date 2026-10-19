# C başlıkları

RadioS modüllerinin ortak C arayüzleri `radios/` altındadır. Aşama 3'te yapılandırma, çalışma kuyruğu, kaynak denetimi ve pencere başlıkları eklendi. Bellek/veri sahipliği ve kapanış sözleşmeleri ilgili başlıkta açıklanır; ABI henüz kararlı bir dış eklenti sözleşmesi değildir.

Aşama 6 ile `data_client.h` kimlikli bloklayan worker HTTP istemcisini; `data.h` native asenkron istek, istasyon bağlamı, yenileme ve CAS belge akışını tanımlar. GTK callback yalnız yaşayan sahibi için ana akışta çalışır; JSON sonuçları belirtilen sahiplik sözleşmesine göre serbest bırakılır.
