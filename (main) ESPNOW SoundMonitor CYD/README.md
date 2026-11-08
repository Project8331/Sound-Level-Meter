# ESP-NOW Sound Monitor — CYD ESP32-2432S028R

Firmware terpisah untuk CYD 2.8 inci (320×240, ILI9341). CYD bertindak sebagai **receiver ESP-NOW + display + SD logger** untuk enam pengirim dan satu node receiver yang mengirim nilai dB.

## Fitur

- Loading screen saat startup.
- Home screen sederhana dengan P1–P7 di tengah gauge, nilai dB di sebelah kiri, serta judul `KEBISINGAN (dB)`. Warna gauge berubah sesuai tingkat kebisingan:
  - biru: `< 50 dB`
  - hijau: `50–<65 dB`
  - kuning: `65–<80 dB`
  - merah: `80–<95 dB`
  - hitam: `>= 95 dB`
- Sentuh kartu untuk membuka detail `Pengirim 1`–`Pengirim 7`: nilai saat ini, maksimum sejak boot beserta waktunya, chart 24 sampel, dan dua catatan terbaru. Tombol kembali menggunakan `<<`.
- Tombol `Status >>` membuka status ESP-NOW, RTC, SD Card, dan Pengirim 1–7. Pengirim ditandai READY jika paket diterima dalam 5 detik terakhir.
- Sampel disimpan pada microSD dalam tujuh blok CSV (`timestamp`, `source`, `db`) sesuai template; data dari beberapa pengirim yang datang berdekatan digabung ke satu baris.
- Timestamp berasal dari RTC DS3231. Jika RTC kehilangan daya, firmware menyetel RTC dari waktu kompilasi (`__DATE__`/`__TIME__`) saat `SET_RTC_FROM_BUILD_TIME_IF_LOST` aktif; periksa waktu dan zona waktu komputer saat build. Jika RTC tidak terdeteksi atau opsi tersebut dimatikan, log tetap masuk tetapi memakai `RTC_UNSET+...` (waktu sejak boot), bukan waktu kalender yang akurat.

## PlatformIO

Buka folder ini sebagai project PlatformIO. Build/upload environment `cyd_esp32_2432s028r`. Library ditarik oleh PlatformIO dari `platformio.ini`.

## Konfigurasi MAC dan payload ESP-NOW

Edit `SOURCE_MACS` pada `src/main.cpp`:

- Slot 1–6: MAC keenam pengirim.
- Slot 7 (`Receiver`): MAC node receiver yang mengirim pengukuran mic-nya ke CYD.
- Semua alamat di repository diset ke nol sebagai placeholder. Isi MAC STA perangkat yang digunakan; paket dari MAC yang belum didaftarkan akan diabaikan.

Firmware menerima dua bentuk paket:

1. **Langsung:** satu `float` berisi dB (4 byte), sama dengan `struct_message { float dB; }` dari sketch awal. MAC pengirim menentukan kartu mana yang diperbarui.
2. **Agregat dari receiver:** tujuh `float` berurutan (28 byte), yaitu pengirim 1–6 lalu nilai mic receiver. Paket agregat hanya diterima dari MAC pada slot `Receiver`. Kanal `NaN` dari receiver ditandai tidak tersedia dan tidak dicatat sebagai sampel.

Topologi utama repository ini memakai paket agregat: Receiver_v2 mengumpulkan data enam transmitter, menambahkan nilai mic lokal, lalu mengirim array tujuh float ke CYD. Daftarkan MAC receiver di slot 7 dan pastikan urutannya sama dengan kartu di layar. Firmware CYD juga dapat menerima paket langsung satu float dari transmitter jika topologi tersebut dipilih.

Firmware CYD mengaktifkan ESP-NOW Long Range pada channel 1. Pastikan semua transmitter dan Receiver juga memakai Long Range/channel 1. CYD mencetak MAC-nya sendiri saat startup; node yang mengirim unicast harus mendaftarkan MAC CYD itu sebagai peer.

Contoh format payload agregat pada node receiver:

```cpp
struct SoundPacket {
  float db[7]; // pengirim 1..6, lalu receiver (mic lokal)
};
SoundPacket packet = {{db1, db2, db3, db4, db5, db6, localMicDb}};
esp_now_send(cydMac, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
```

Gunakan ESP-NOW Long Range pada channel 1 untuk CYD, semua transmitter, dan Receiver. MAC perangkat pada source code repository ini adalah placeholder; isi alamat yang benar saat menyiapkan perangkat.

## RTC DS3231, SD, dan pin

Pin yang digunakan oleh firmware ini:

| Modul | Pin ESP32 |
|---|---|
| DS3231 SDA / SCL | GPIO 22 / GPIO 27 |
| microSD CS / SCK / MISO / MOSI | GPIO 5 / 18 / 19 / 23 |
| Touch IRQ / MOSI / MISO / CLK / CS | GPIO 36 / 32 / 39 / 25 / 33 |
| Backlight TFT | GPIO 21 |

Pin bisa diubah di konstanta bagian atas `src/main.cpp`. RTC memakai GPIO pada tabel di atas. Secara default, kehilangan daya memicu setelan dari waktu kompilasi; setelah startup, cek RTC pada Serial Monitor dan pastikan komputer build memakai waktu/zona waktu yang benar. Matikan `SET_RTC_FROM_BUILD_TIME_IF_LOST` jika ingin mengatur DS3231 secara eksternal. Jika modul RTC berbeda, sesuaikan inisialisasi `RTC_DS3231`.

CSV mengikuti template tujuh blok: baris pertama berisi `DATA 1` sampai `DATA 7`, baris kedua mengulang kolom `timestamp,source,db` untuk tiap blok, dan satu kolom kosong memisahkan blok. Setiap sampel ditempatkan pada blok sumbernya; sampel dengan timestamp sama yang diterima dalam rentang 150 ms digabung ke satu baris. File baru dibuat sebagai `/noise.csv`. Jika `/noise.csv` lama memakai format `timestamp,source,db`, file tersebut tidak ditimpa atau dicampur; firmware menyimpan format baru di `/noise_wide.csv`. File disimpan pada microSD dengan CS GPIO 5. TFT memakai HSPI, sedangkan SD dan touch memakai VSPI yang pin-nya dipetakan ulang saat akses; firmware menangani pergantian rute tersebut. Jika memakai varian CYD berbeda, cek ulang pin dan SPI routing sebelum upload.

## Catatan ambang kebisingan

Garis acuan yang ditampilkan adalah **85 dBA selama 8 jam**, sebuah acuan paparan kerja yang lazim dipakai, bukan satu angka batas maksimum universal untuk semua orang/situasi. Risiko bergantung pada durasi, karakter suara, dan aturan setempat. Batas warna gauge merupakan kategori tampilan dan dapat diubah melalui `colorForDb()`; kalibrasi sensor dan hasil dB(A) juga perlu diverifikasi di perangkat nyata.
