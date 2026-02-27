const express = require('express');
const multer  = require('multer');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');

const app = express();
// A felhő szolgáltatók (mint a Render) egy automatikus PORT változót adnak. 
// Ha az nincs, akkor marad a jó öreg 3000-es port a saját gépeden.
const port = process.env.PORT || 3000; 

// --- 1. BIZTONSÁGI INTÉZKEDÉS: Mappa létrehozása ---
// Mivel a felhőbe csak a kódot töltjük fel, az 'uploads' mappa nem fog létezni. Létrehozzuk!
const uploadsDir = path.join(__dirname, 'uploads');
if (!fs.existsSync(uploadsDir)) {
    fs.mkdirSync(uploadsDir);
}

const upload = multer({ dest: 'uploads/' });
app.use(express.static(path.join(__dirname, 'public')));

app.post('/encode', upload.single('image'), (req, res) => {
    if (!req.file || !req.body.message) {
        return res.status(400).send('Hiányzó fájl vagy üzenet!');
    }

    const inputPath = req.file.path;
    const secretMessage = req.body.message;
    
    const outputPath = path.join(__dirname, 'uploads', `secret_${Date.now()}.png`); 
    const textPath = path.join(__dirname, 'uploads', `msg_${Date.now()}.txt`); 
    
    fs.writeFileSync(textPath, secretMessage, 'utf8');

    // --- 2. AZ OKOS RENDSZER-FELISMERŐ LOGIKA ---
    // process.platform megmondja, hol vagyunk: 'win32' = Windows, 'linux' = Linux szerver
    const isWindows = process.platform === 'win32';
    
    // Ha Windows, kell a .exe. Ha Linux, akkor csak simán a fájl neve!
    const exeName = isWindows ? 'png_readerFINAL.exe' : 'png_readerFINAL';
    const exePath = path.join(__dirname, exeName);

    console.log(`Titkosítás indítása (${exeName})... Kép: ${inputPath}`);

    execFile(exePath, [inputPath, outputPath, textPath], (error, stdout, stderr) => {
        if (error) {
            console.error("\n[Rendszerhiba]:", error.message);
            console.error("[C kimenet]:\n", stdout);
            return res.status(500).send('Hiba történt a titkosítás során.');
        }

        console.log("Titkosítás sikeres! Fájl küldése a böngészőnek...");

        res.download(outputPath, 'titkos_kep.png', (err) => {
            fs.unlink(inputPath, () => {});
            fs.unlink(outputPath, () => {});
            fs.unlink(textPath, () => {});
        });
    });
});

app.listen(port, () => {
    console.log(`\n🚀 A szerver elindult a ${port}-es porton!`);
});