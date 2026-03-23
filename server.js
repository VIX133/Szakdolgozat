const express = require('express');
const multer  = require('multer');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');

const app = express();
// A felhő szolgáltatók (mint a Render) egy automatikus PORT változót adnak. 
// Ha az nincs, akkor marad a jó öreg 3000-es port a saját gépeden.
const port = process.env.PORT || 3000; 

// Mivel a felhőbe csak a kódot töltjük fel, az 'uploads' mappa nem fog létezni. Létrehozzuk!
const uploadsDir = path.join(__dirname, 'uploads');
if (!fs.existsSync(uploadsDir)) {
    fs.mkdirSync(uploadsDir);
}

// Multer beállítása a fájlok mentési helyéhez
const upload = multer({ dest: 'uploads/' });
app.use(express.static(path.join(__dirname, 'public')));

// --- ITT A VÁLTOZÁS: Két külön fájlt várunk (image és secretFile) ---
app.post('/encode', upload.fields([
    { name: 'image', maxCount: 1 },
    { name: 'secretFile', maxCount: 1 }
]), (req, res) => {
    
    // Ellenőrizzük, hogy mindkét fájl sikeresen megérkezett-e a böngészőtől
    if (!req.files || !req.files['image'] || !req.files['secretFile']) {
        return res.status(400).send('Hiányzik a kép vagy a rejtendő fájl!');
    }

    // Kinyerjük az útvonalakat az 'uploads' mappából
    const inputPath = req.files['image'][0].path;
    const secretFilePath = req.files['secretFile'][0].path; 
    const originalFileName = req.files['secretFile'][0].originalname;
    
    // Ide fogja generálni a C program a kész képet
    const outputPath = path.join(__dirname, 'uploads', `secret_${Date.now()}.png`); 

    const isWindows = process.platform === 'win32';
    
    // A C programod neve (hagytam az eredetit)
    const exeName = isWindows ? 'png_readerFINAL.exe' : 'png_readerFINAL';
    const exePath = path.join(__dirname, exeName);

    console.log(`Titkosítás indítása (${exeName})... Kép: ${inputPath}, Fájl: ${secretFilePath}`);

    // --- C MOTOR MEGHÍVÁSA ---
    // Sorrend: 1. Bemeneti kép, 2. Kimeneti kép, 3. Rejtendő fájl
    execFile(exePath, [inputPath, outputPath, secretFilePath,originalFileName], (error, stdout, stderr) => {
        if (error) {
            console.error("\n[Rendszerhiba]:", error.message);
            console.error("[C kimenet]:\n", stdout);
            
            // Hiba esetén is letöröljük a feltöltött fájlokat!
            fs.unlink(inputPath, () => {});
            fs.unlink(secretFilePath, () => {});
            return res.status(500).send('Hiba történt a titkosítás során a C motorban.');
        }

        console.log("Titkosítás sikeres! Fájl küldése a böngészőnek...");

        // Ha minden jó, visszaküldjük a módosított PNG-t a felhasználónak letöltésre
        res.download(outputPath, 'titkos_kep.png', (err) => {
            if (err) console.error("Hiba a letöltésnél:", err);
            
            // --- TAKARÍTÁS: Törlünk mindent a lemezről ---
            fs.unlink(inputPath, () => {});
            fs.unlink(outputPath, () => {});
            fs.unlink(secretFilePath, () => {}); // A feltöltött titkos fájlt is!
        });
    });
});

app.listen(port, () => {
    console.log(`\n🚀 A szerver elindult a ${port}-es porton!`);
});