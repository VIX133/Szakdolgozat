#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

struct Header {
    uint32_t size;   // rejtett adat hossza
    char ext[4];     // pl. "txt"
};

void hide_bit_in_byte(uint8_t *byte, uint8_t bit) {
    *byte = (*byte & 0xFE) | (bit & 1);
}

void hide_data_in_image(uint8_t *decompressed, uint32_t width, uint32_t height, const void *data, size_t len_bytes, size_t *current_bit_pos) {
    uint32_t bpp = 4;
    uint32_t row_size = width * bpp + 1;

    const uint8_t *bytes = (const uint8_t*)data;

    size_t global_bit_pos = *current_bit_pos;

    for (size_t i = 0; i < len_bytes; i++) {
        // minden bájthoz 8 csatorna kell
        for (int j = 7; j >= 0; --j) {
            uint8_t bit = (bytes[i] >> j) & 1;

            // bit_pos → (y,x,csatorna) kiszámítása
            uint32_t channel_index = (uint32_t)global_bit_pos;
            uint32_t y = channel_index / (width * bpp);
            uint32_t x_in_row = channel_index % (width * bpp);

            if (y >= height) {
                // nincs több hely
                return;
            }

            uint8_t *scanline = decompressed + y * row_size;
            uint8_t *line = scanline + 1; // filter kihagyása

            hide_bit_in_byte(&line[x_in_row], bit);

            global_bit_pos++;
        }
    }
    *current_bit_pos = global_bit_pos;
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
    len_bytes[2] = (len >> 8)  & 0xFF;
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
    // A zlib crc32 függvényét használjuk
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
    crc_bytes[2] = (crc >> 8)  & 0xFF;
    crc_bytes[3] = crc & 0xFF;
    fwrite(crc_bytes, 1, 4, f);
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        printf("Hiba: Nem megfelelő parameterek!\n");
        printf("Hasznalat: %s <bemeneti_kep.png> <kimeneti_kep.png> \"<titkos uzenet>\"\n", argv[0]);
        return 1;
    }

    const char *input_filename = argv[1];
    const char *output_filename = argv[2];
    const char *text_filename = argv[3];

    // --- SZÖVEG BEOLVASÁSA A FÁJLBÓL (UTF-8 ÉRINTETLENÜL HAGYÁSA) ---
    FILE *msg_file = fopen(text_filename, "rb");
    if (!msg_file) {
        printf("Hiba: Nem talalom a szoveges fajlt: %s\n", text_filename);
        return 1;
    }

    // Megnézzük, milyen hosszú a szöveg a fájlban
    fseek(msg_file, 0, SEEK_END);
    long msg_size = ftell(msg_file);
    fseek(msg_file, 0, SEEK_SET);

    // Lefoglaljuk neki a memóriát, és beolvassuk
    char *secret_message = malloc(msg_size + 1);
    fread(secret_message, 1, msg_size, msg_file);
    secret_message[msg_size] = '\0'; // Sztring lezárása
    fclose(msg_file);

    // File beolvasas
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
    if (signature[0] != 0x89 || signature[1] != 'P' || signature[2] != 'N' || signature[3] != 'G' || signature[4] != 0x0D ||
        signature[5] != 0x0A || signature[6] != 0x1A || signature[7] != 0x0A)
    {
        printf("Ez NEM PNG fajl!\n");
        fclose(f);
        return 1;
    }
    printf("PNG signature OK\n");

    // IHDR chunk: hossz
    uint8_t len_bytes[4];
    fread(len_bytes, 1, 4, f);
    uint32_t ihdr_len = (len_bytes[0] << 24) | (len_bytes[1] << 16) | (len_bytes[2] << 8) | len_bytes[3];

    if (ihdr_len != 13)
    {
        printf("Hibas IHDR hossz!\n");
        fclose(f);
        return 1;
    }

    // IHDR típus ellenőrzés ("IHDR")
    uint8_t ihdr_type[4];
    fread(ihdr_type, 1, 4, f);
    if (strncmp((char *)ihdr_type, "IHDR", 4) != 0)
    {
        printf("Nincs IHDR chunk!\n");
        fclose(f);
        return 1;
    }
    printf("IHDR chunk OK\n");

    // IHDR adatok: width, height
    uint8_t width_bytes[4], height_bytes[4],bit_depth_bytes[1], color_type_bytes[1],compression_bytes[1], filter_method_bytes[1], interlace_bytes[1] ;
    fread(width_bytes, 1, 4, f);
    fread(height_bytes, 1, 4, f);
    fread(bit_depth_bytes, 1, 1, f);
    fread(color_type_bytes, 1, 1, f);
    fread(compression_bytes, 1, 1, f);
    fread(filter_method_bytes, 1, 1, f);
    fread(interlace_bytes, 1, 1, f);
    uint32_t width = (width_bytes[0] << 24) | (width_bytes[1] << 16) | (width_bytes[2] << 8) | width_bytes[3];
    uint32_t height = (height_bytes[0] << 24) | (height_bytes[1] << 16) | (height_bytes[2] << 8) | height_bytes[3];

    printf("Kep merete: %u x %u pixel\n", width, height);

    //CRC átugras
    fseek(f, 4, SEEK_CUR);

    uint8_t *compressed = NULL;
    size_t compressed_size = 0;

    // IDAT-okat ciklussal vegigjárom, és egy compressed dinamikus tömbbe teszem
    while (1)
    {
        uint8_t len_bytes[4], type[4];

        // 1) Hossz
        if (fread(len_bytes, 1, 4, f) != 4)
            break;
        uint32_t length = (len_bytes[0] << 24) | (len_bytes[1] << 16) | (len_bytes[2] << 8) | len_bytes[3];

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
        return 1;
    }

    // Zlibbel kitomoritem a tombot az adatot.
    uLongf decompressed_size = width * height * bpp + height; // RGBA + 1 filter/sor
    uint8_t *decompressed = malloc(decompressed_size);
    int zerr = uncompress(decompressed, &decompressed_size, compressed, compressed_size);

    if (zerr != Z_OK)
    {
        printf("Zlib hiba: %d\n", zerr);
        free(compressed);
        free(decompressed);
        fclose(f);
        return 1;
    }

    printf("Kitomoritve: %lu byte (raw pixels + filter)\n", decompressed_size);

    uint32_t stride = 1 + width * bpp;
    for (uint32_t y = 0; y < height; y++)
    {
        // Aktuális sor mutatója
        uint8_t *current_scanline = decompressed + y * stride;

        // Előző sor mutatója (az első sornál NULL)
        uint8_t *prev_scanline = NULL;
        if (y > 0)
        {
            prev_scanline = decompressed + (y - 1) * stride;
        }

        // Defilter meghívása az aktuális sorra
        png_defilter(current_scanline, prev_scanline, width, bpp);

        current_scanline[0] = 0;
    }
    printf("Defiltering kesz!\n");

    struct Header header;

    header.size = (uint32_t)strlen(secret_message);
    strncpy(header.ext, "txt", 4);

    // 2. Kapacitás ellenőrzés (Header + Üzenet együtt)
    size_t total_bits_needed = (sizeof(struct Header) + header.size) * 8;
    if (total_bits_needed > width * height * bpp * 8) {
        printf("Nem fer bele!\n");
        return 1;
    }

    size_t current_pos = 0;
    //Eloszor elrejtem a headert
    hide_data_in_image(decompressed, width, height, &header, sizeof(struct Header), &current_pos);
    //Utanna az uzenetet
    hide_data_in_image(decompressed, width, height, secret_message, header.size, &current_pos);

    printf("Minden adat elrejtve! Utolso bit pozicioja: %zu\n", current_pos);

    // --- 4. LÉPÉS: VISSZATÖMÖRÍTÉS ÉS MENTÉS ---

    printf("Adatok visszatomoritese...\n");

    // Kiszámoljuk, mekkora puffer kell a tömörítéshez (biztonsági ráhagyással)
    uLongf new_compressed_size = compressBound(decompressed_size);
    uint8_t *new_compressed = malloc(new_compressed_size);

    if (!new_compressed)
    {
        printf("Memoriahiba a tomoritesnel!\n");
        return 1;
    }

    // Tényleges tömörítés (decompressed -> new_compressed)
    if (compress(new_compressed, &new_compressed_size, decompressed, decompressed_size) != Z_OK)
    {
        printf("Hiba a compress hivasnal!\n");
        free(new_compressed);
        return 1;
    }
    printf("Uj IDAT meret: %lu bajt\n", new_compressed_size);

    FILE *out = fopen(output_filename, "wb");
    if (!out)
    {
        printf("Nem tudom megnyitni irasra a secret.png-t!\n");
        return 1;
    }

    // A) PNG Szignatúra (Magic Bytes)
    uint8_t png_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(png_signature, 1, 8, out);

    // B) IHDR Chunk (Kép paraméterei)
    // Összeállítjuk a 13 bájtos IHDR adatot
    uint8_t ihdr_data[13];
    // Width
    ihdr_data[0] = (width >> 24) & 0xFF; ihdr_data[1] = (width >> 16) & 0xFF;
    ihdr_data[2] = (width >> 8)  & 0xFF; ihdr_data[3] = width & 0xFF;
    // Height
    ihdr_data[4] = (height >> 24) & 0xFF; ihdr_data[5] = (height >> 16) & 0xFF;
    ihdr_data[6] = (height >> 8)  & 0xFF; ihdr_data[7] = height & 0xFF;
    ihdr_data[8] = 8;  // Bit depth (8 bit/csatorna)
    ihdr_data[9] = color_type_bytes[0];  // Color type (RGBA) 
    ihdr_data[10] = 0; // Compression (Deflate)
    ihdr_data[11] = 0; // Filter method
    ihdr_data[12] = 0; // Interlace (Nincs)

    write_chunk(out, "IHDR", ihdr_data, 13);

    // C) IDAT Chunk (A képadatok)
    write_chunk(out, "IDAT", new_compressed, new_compressed_size);

    // D) IEND Chunk (Lezárás - üres adat)
    write_chunk(out, "IEND", NULL, 0);

    fclose(out);
    printf("SIKER! A titkos kep elmentve: secret.png\n");

    // Takarítás
    free(compressed);
    free(decompressed);
    free(new_compressed);
    free(secret_message);
    fclose(f);

    return 0;
}
