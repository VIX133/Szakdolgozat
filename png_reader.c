#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

// ÚJ FEJLÉC: Fájlnév tárolása a 4 karakteres kiterjesztés helyett
struct Header {
    uint32_t size;       // rejtett fájl hossza
    char filename[60];   // pl. "titkos_szerzodes.pdf"
};

// ÚJ 2-BITES BEÁGYAZÓ FÜGGVÉNY
void hide_twobits_in_byte(uint8_t *byte, uint8_t bits) {
    // 0xFC (252) maszk kinullázza az utolsó 2 bitet, utána rátesszük a mi 2 bitünket
    *byte = (*byte & 0xFC) | (bits & 0x03);
}

// ÚJ LSB-2 MOTOR: Alpha csatorna kihagyásával
void hide_data_in_image(uint8_t *decompressed,
                        uint32_t width,
                        uint32_t height,
                        uint32_t bpp,
                        const void *data,
                        size_t len_bytes,
                        size_t *current_channel_idx) {
    uint32_t row_size = width * bpp + 1; // 1 bájt filter + pixelek
    const uint8_t *bytes = (const uint8_t*)data;
    size_t idx = *current_channel_idx;

    for (size_t i = 0; i < len_bytes; i++) {
        // Egy 8 bites bájt elrejtéséhez 4-szer fut le (2 bitenként)
        for (int shift = 6; shift >= 0; shift -= 2) {

            uint32_t x_in_row = idx % (width * bpp);

            // ALPHA UGRÁS: Ha RGBA kép (bpp == 4) és ez az Alpha csatorna
            if (bpp == 4 && (x_in_row % 4) == 3) {
                idx++; // Átugorjuk az Alpha bájtot!
                x_in_row = idx % (width * bpp); // Újraszámoljuk a sorbeli pozíciót
            }

            uint32_t y = idx / (width * bpp);

            if (y >= height) {
                return; // Nincs több hely a képben
            }

            // Megkeressük a pontos memóriacímet a sorban (filter bájt átugrásával)
            uint8_t *scanline = decompressed + y * row_size;
            uint8_t *line = scanline + 1;

            // 2 bit kivágása a rejtendő fájl bájtjából
            uint8_t two_bits = (bytes[i] >> shift) & 0x03;

            // Beágyazás
            hide_twobits_in_byte(&line[x_in_row], two_bits);

            idx++; // Lépünk a következő színcsatornára
        }
    }
    *current_channel_idx = idx; // Elmentjük, hol hagytuk abba
}

uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c)
{
    int32_t p = a + b - c;
    int32_t pa = abs(p - a);
    int32_t pb = abs(p - b);
    int32_t pc = abs(p - c);
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

void png_defilter(uint8_t *scanline, uint8_t *prev_scanline, uint32_t width, uint8_t bpp)
{
    uint8_t filter = scanline[0];
    uint8_t *line = scanline + 1; // első bájt a filter

    if (filter == 0)
        return; // None - nincs teendő

    for (uint32_t x = 0; x < width * bpp; x++)
    {
        uint8_t a = 0;
        if (x >= bpp)
        {
            a = line[x - bpp]; // bal szomszéd
        }

        uint8_t b = 0;
        if (prev_scanline != NULL)
        {
            b = prev_scanline[x + 1]; // felül
        }

        uint8_t c = 0;
        if (x >= bpp && prev_scanline != NULL)
        {
            c = prev_scanline[x - bpp + 1]; // bal-felül
        }

        switch (filter)
        {
        case 1: // Sub
            line[x] += a;
            break;
        case 2: // Up
            line[x] += b;
            break;
        case 3: // Average
            line[x] += (a + b) / 2;
            break;
        case 4: // Paeth
            line[x] += paeth_predictor(a, b, c);
            break;
        }
    }
}

// Segédfüggvény chunkok írásához
void write_chunk(FILE *f, const char *type, uint8_t *data, uint32_t len)
{
    // 1. Hossz kiírása (4 bájt, Big-Endian)
    uint8_t len_bytes[4];
    len_bytes[0] = (len >> 24) & 0xFF;
    len_bytes[1] = (len >> 16) & 0xFF;
    len_bytes[2] = (len >> 8) & 0xFF;
    len_bytes[3] = len & 0xFF;
    fwrite(len_bytes, 1, 4, f);

    // 2. Típus kiírása (pl. "IDAT")
    fwrite(type, 1, 4, f);

    // 3. Adat kiírása (ha van)
    if (len > 0 && data != NULL)
    {
        fwrite(data, 1, len, f);
    }

    // 4. CRC számítás (Típus + Adat)
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const unsigned char *)type, 4);
    if (len > 0 && data != NULL)
    {
        crc = crc32(crc, data, len);
    }

    // 5. CRC kiírása (4 bájt, Big-Endian)
    uint8_t crc_bytes[4];
    crc_bytes[0] = (crc >> 24) & 0xFF;
    crc_bytes[1] = (crc >> 16) & 0xFF;
    crc_bytes[2] = (crc >> 8) & 0xFF;
    crc_bytes[3] = crc & 0xFF;
    fwrite(crc_bytes, 1, 4, f);
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        printf("Hiba: Nem megfelelő parameterek!\n");
        printf("Hasznalat: %s <bemeneti_png> <kimeneti_png> <rejtett_fajl>\n", argv[0]);
        return 1;
    }

    const char *input_filename  = argv[1];
    const char *output_filename = argv[2];
    const char *text_filename   = argv[3];
    const char *original_name = (argc >= 5) ? argv[4] : text_filename;

    // --- SZÖVEG / BINÁRIS FÁJL BEOLVASÁSA ---
    FILE *msg_file = fopen(text_filename, "rb");
    if (!msg_file) {
        printf("Hiba: Nem talalom a szoveges/binaris fajlt: %s\n", text_filename);
        return 1;
    }

    fseek(msg_file, 0, SEEK_END);
    long msg_size = ftell(msg_file);
    fseek(msg_file, 0, SEEK_SET);

    char *secret_message = malloc(msg_size);
    if (!secret_message) {
        printf("Memoriahiba a rejtett fajl betoltesekor!\n");
        fclose(msg_file);
        return 1;
    }

    fread(secret_message, 1, msg_size, msg_file);
    fclose(msg_file);

    // PNG beolvasás
    FILE *f = fopen(input_filename, "rb");
    if (!f)
    {
        printf("Nem találom ezt a képet: %s\n", input_filename);
        free(secret_message);
        return 1;
    }

    // PNG signature ellenőrzés (8 bájt)
    uint8_t signature[8];
    fread(signature, 1, 8, f);
    if (signature[0] != 0x89 || signature[1] != 'P' || signature[2] != 'N' || signature[3] != 'G' ||
        signature[4] != 0x0D || signature[5] != 0x0A || signature[6] != 0x1A || signature[7] != 0x0A)
    {
        printf("Ez NEM PNG fajl!\n");
        fclose(f);
        free(secret_message);
        return 1;
    }
    printf("PNG signature OK\n");

    // IHDR chunk: hossz
    uint8_t len_bytes[4];
    fread(len_bytes, 1, 4, f);
    uint32_t ihdr_len = (len_bytes[0] << 24) | (len_bytes[1] << 16) |
                        (len_bytes[2] << 8)  |  len_bytes[3];

    if (ihdr_len != 13)
    {
        printf("Hibas IHDR hossz!\n");
        fclose(f);
        free(secret_message);
        return 1;
    }

    // IHDR típus ellenőrzés ("IHDR")
    uint8_t ihdr_type[4];
    fread(ihdr_type, 1, 4, f);
    if (strncmp((char *)ihdr_type, "IHDR", 4) != 0)
    {
        printf("Nincs IHDR chunk!\n");
        fclose(f);
        free(secret_message);
        return 1;
    }
    printf("IHDR chunk OK\n");

    // IHDR adatok: width, height, stb.
    uint8_t width_bytes[4], height_bytes[4],
            bit_depth_bytes[1], color_type_bytes[1],
            compression_bytes[1], filter_method_bytes[1],
            interlace_bytes[1];

    fread(width_bytes, 1, 4, f);
    fread(height_bytes, 1, 4, f);
    fread(bit_depth_bytes, 1, 1, f);
    fread(color_type_bytes, 1, 1, f);
    fread(compression_bytes, 1, 1, f);
    fread(filter_method_bytes, 1, 1, f);
    fread(interlace_bytes, 1, 1, f);

    uint32_t width  = (width_bytes[0]  << 24) | (width_bytes[1]  << 16) |
                      (width_bytes[2]  <<  8) |  width_bytes[3];
    uint32_t height = (height_bytes[0] << 24) | (height_bytes[1] << 16) |
                      (height_bytes[2] <<  8) |  height_bytes[3];

    printf("Kep merete: %u x %u pixel\n", width, height);

    // CRC átugrás
    fseek(f, 4, SEEK_CUR);

    uint8_t *compressed = NULL;
    size_t compressed_size = 0;

    // IDAT-okat ciklussal vegigjárom, és egy compressed dinamikus tömbbe teszem
    while (1)
    {
        uint8_t len_bytes2[4], type[4];

        // 1) Hossz
        if (fread(len_bytes2, 1, 4, f) != 4)
            break;
        uint32_t length =
            (len_bytes2[0] << 24) | (len_bytes2[1] << 16) |
            (len_bytes2[2] <<  8) |  len_bytes2[3];

        // 2) Típus
        if (fread(type, 1, 4, f) != 4)
            break;

        // Adatok compressed tömbbe rakom
        if (!strncmp((char *)type, "IDAT", 4))
        {
            compressed = realloc(compressed, compressed_size + length);
            fread(compressed + compressed_size, 1, length, f);
            compressed_size += length;
        }
        else if (!strncmp((char *)type, "IEND", 4))
        {
            break;
        }
        else
        {
            fseek(f, length, SEEK_CUR);
        }
        // Minden chunk végén 4 bájt CRC-t át kell ugrani:
        fseek(f, 4, SEEK_CUR);
    }

    printf("Osszes IDAT: %zu bajt\n", compressed_size);

    uint32_t bpp;
    if (color_type_bytes[0] == 6) {
        bpp = 4; // RGBA
    } else if (color_type_bytes[0] == 2) {
        bpp = 3; // RGB
    } else {
        printf("Hiba: Csak RGB vagy RGBA PNG tamogatott!\n");
        free(compressed);
        free(secret_message);
        fclose(f);
        return 1;
    }

    // Zlibbel kitomoritem a tombot
    uLongf decompressed_size = width * height * bpp + height; // RGBA + 1 filter/sor
    uint8_t *decompressed = malloc(decompressed_size);
    if (!decompressed) {
        printf("Memoriahiba a kitomoritesnel!\n");
        free(compressed);
        free(secret_message);
        fclose(f);
        return 1;
    }

    int zerr = uncompress(decompressed, &decompressed_size, compressed, compressed_size);
    if (zerr != Z_OK)
    {
        printf("Zlib hiba: %d\n", zerr);
        free(compressed);
        free(decompressed);
        free(secret_message);
        fclose(f);
        return 1;
    }

    printf("Kitomoritve: %lu byte (raw pixels + filter)\n", decompressed_size);

    uint32_t stride = 1 + width * bpp;
    for (uint32_t y = 0; y < height; y++)
    {
        uint8_t *current_scanline = decompressed + y * stride;
        uint8_t *prev_scanline = NULL;
        if (y > 0)
        {
            prev_scanline = decompressed + (y - 1) * stride;
        }

        png_defilter(current_scanline, prev_scanline, width, bpp);

        current_scanline[0] = 0;
    }
    printf("Defiltering kesz!\n");

    // --- ÚJ FEJLÉC ÖSSZEÁLLÍTÁSA ---
    struct Header header = {0};
    header.size = (uint32_t)msg_size; // A beolvasott fájl (pl. PDF) tényleges mérete

    // Fájlnév kinyerése AZ EREDETI NÉVBŐL (4. paraméter / GUI név)
    const char *filename_only = original_name;
    const char *slash = strrchr(filename_only, '/');
    if (slash) filename_only = slash + 1;
    const char *backslash = strrchr(filename_only, '\\');
    if (backslash) filename_only = backslash + 1;

strncpy(header.filename, filename_only, 59);
header.filename[59] = '\0';

    // --- ÚJ LSB-2 KAPACITÁS ELLENŐRZÉS ---
    size_t total_bytes_to_hide = sizeof(struct Header) + header.size;
    size_t channels_needed = total_bytes_to_hide * 4;

    // Rendelkezésre álló csatornák (Az Alpha bájtokat nem számoljuk bele!)
    size_t available_channels = width * height * 3;

    if (channels_needed > available_channels) {
        printf("Nem fer bele a fajl a kepbe! (Tul nagy)\n");
        free(compressed);
        free(decompressed);
        free(secret_message);
        fclose(f);
        return 1;
    }

    // --- BEÁGYAZÁS ---
    size_t current_pos = 0;

    // 1. Először a headert
    hide_data_in_image(decompressed, width, height, bpp, &header, sizeof(struct Header), &current_pos);

    // 2. Utána magát a fájlt
    hide_data_in_image(decompressed, width, height, bpp, secret_message, header.size, &current_pos);

    printf("Minden adat elrejtve! Elhasznalt csatornak: %zu\n", current_pos);

    printf("Adatok visszatomoritese...\n");
    uLongf new_compressed_size = compressBound(decompressed_size);
    uint8_t *new_compressed = malloc(new_compressed_size);
    if (!new_compressed)
    {
        printf("Memoriahiba a tomoritesnel!\n");
        free(compressed);
        free(decompressed);
        free(secret_message);
        fclose(f);
        return 1;
    }

    if (compress(new_compressed, &new_compressed_size, decompressed, decompressed_size) != Z_OK)
    {
        printf("Hiba a compress hivasnal!\n");
        free(new_compressed);
        free(compressed);
        free(decompressed);
        free(secret_message);
        fclose(f);
        return 1;
    }
    printf("Uj IDAT meret: %lu bajt\n", new_compressed_size);

    FILE *out = fopen(output_filename, "wb");
    if (!out)
    {
        printf("Nem tudom megnyitni irasra a kimeneti PNG-t!\n");
        free(new_compressed);
        free(compressed);
        free(decompressed);
        free(secret_message);
        fclose(f);
        return 1;
    }

    // A) PNG Szignatúra (Magic Bytes)
    uint8_t png_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(png_signature, 1, 8, out);

    // B) IHDR Chunk (Kép paraméterei)
    uint8_t ihdr_data[13];
    ihdr_data[0] = (width >> 24) & 0xFF;
    ihdr_data[1] = (width >> 16) & 0xFF;
    ihdr_data[2] = (width >> 8)  & 0xFF;
    ihdr_data[3] =  width        & 0xFF;

    ihdr_data[4] = (height >> 24) & 0xFF;
    ihdr_data[5] = (height >> 16) & 0xFF;
    ihdr_data[6] = (height >> 8)  & 0xFF;
    ihdr_data[7] =  height        & 0xFF;

    ihdr_data[8]  = 8;                 // Bit depth (8 bit/csatorna)
    ihdr_data[9]  = color_type_bytes[0]; // Color type (RGB/RGBA ugyanaz, mint bemenet)
    ihdr_data[10] = 0;                 // Compression (Deflate)
    ihdr_data[11] = 0;                 // Filter method
    ihdr_data[12] = 0;                 // Interlace (Nincs)

    write_chunk(out, "IHDR", ihdr_data, 13);

    // C) IDAT Chunk (A képadatok)
    write_chunk(out, "IDAT", new_compressed, new_compressed_size);

    // D) IEND Chunk (Lezárás - üres adat)
    write_chunk(out, "IEND", NULL, 0);

    fclose(out);
    printf("SIKER! A titkos kep elmentve: %s\n", output_filename);

    // Takarítás
    free(compressed);
    free(decompressed);
    free(new_compressed);
    free(secret_message);
    fclose(f);

    return 0;
}