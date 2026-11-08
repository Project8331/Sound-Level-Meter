# Receiver v2 — ESP-NOW aggregator

Project ini berisi firmware untuk board ESP32 receiver lama. Receiver menerima nilai dari enam node pengirim, menghitung kebisingan mic I2S lokal sebagai kanal ketujuh, lalu mengirim satu paket agregat ke CYD setiap detik. CYD menangani layar, RTC, dan logging microSD.

## Alur

`6 pengirim (float dB) -> Receiver v2 (gabung 6 data + mic lokal) -> CYD (tampilan + RTC + SD)`

## Konfigurasi sebelum upload

1. Upload firmware CYD dari project `ESPNOW_SoundMonitor_CYD` terlebih dahulu. Buka Serial Monitor CYD dan catat `CYD MAC`.
2. Isi `CYD_MAC` pada `src/main.cpp` Receiver v2 menggunakan MAC CYD tersebut.
3. Isi semua entri `SENDER_MACS` dengan MAC STA keenam transmitter yang digunakan.
4. Upload Receiver v2. Catat `MAC Receiver` dari Serial Monitor.
5. Pada `ESPNOW_SoundMonitor_CYD/src/main.cpp`, isi slot `SOURCE_MACS` nomor 7 (`Receiver`) dengan MAC Receiver v2.
6. Pastikan keenam transmitter, Receiver v2, dan CYD memakai ESP-NOW Long Range pada channel 1.

Receiver v2 mengharapkan setiap node pengirim mengirim payload **satu `float`** berisi dB, sama dengan `struct_message { float dB; }` dari sketch awal. Ke CYD, receiver mengirim tujuh `float` berurutan: Pengirim 1–6, lalu nilai mic lokal dalam dBA.

Nilai transmitter diteruskan tanpa batas bawah/atas selama nilainya finite. Jika transmitter tidak mengirim pembaruan selama 5 detik, kanalnya dikirim sebagai `NaN` agar CYD menandainya tidak tersedia, bukan menampilkan data lama sebagai data terbaru.

Mic lokal memakai I2S 16 kHz/32-bit dan filter pembobotan A. Receiver menghitung dBA dan dB(Z) setiap 500 ms; nilai dBA menjadi kanal ketujuh yang dikirim ke CYD, sedangkan dB(Z) ditampilkan di Serial Monitor. Offset kalibrasi default 0 dB, sehingga hasil harus diverifikasi terhadap sound-level meter sebelum dianggap terkalibrasi.

Mode Long Range dan channel 1 harus dikonfigurasi sama pada semua peer ESP-NOW. Firmware `Transmitter_v2` dan CYD pada workspace ini juga mengaktifkan Long Range/channel 1.

## Build / upload

Buka folder `Receiver_v2` sebagai project PlatformIO. Jalankan `pio run` untuk build dan `pio run -t upload` untuk upload ke board ESP32 receiver. Atur port serial sesuai board jika diperlukan.

## Perangkat keras

Mic I2S tetap memakai pin dari sketch lama: WS GPIO 25, DATA GPIO 33, BCLK GPIO 32. OLED dan SD lama tidak dipakai oleh firmware ini karena tampilan serta pencatatan data dipindahkan ke CYD. Jika tetap ingin mempertahankan OLED/SD lokal, perlu ditambahkan kembali tanpa mengubah format paket agregat.
