const express = require('express');
const multer  = require('multer');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');

const app = express();
const port = process.env.PORT || 3000; 

const uploadsDir = path.join(__dirname, 'uploads');
if (!fs.existsSync(uploadsDir)) {
    fs.mkdirSync(uploadsDir);
}

const upload = multer({ dest: 'uploads/' });
app.use(express.static(path.join(__dirname, 'public')));

function removeAccents(str) {
    return str.normalize("NFD").replace(/[\u0300-\u036f]/g, "");
}

app.post('/encode', upload.fields([
    { name: 'image', maxCount: 1 },
    { name: 'secretFile', maxCount: 1 }
]), (req, res) => {
    
    if (!req.files || !req.files['image']) {
        return res.status(400).send('Hiányzik a sablon kép!');
    }

    const inputPath = req.files['image'][0].path;
    const outputPath = path.join(__dirname, 'uploads', `secret_${Date.now()}.png`); 
    
    let secretFilePath = "";
    let originalFileName = "";

    if (req.files['secretFile']) {
        secretFilePath = req.files['secretFile'][0].path;
        originalFileName = removeAccents(req.files['secretFile'][0].originalname);
    } 

    else if (req.body.message) {
        secretFilePath = path.join(__dirname, 'uploads', `msg_${Date.now()}.txt`);
        fs.writeFileSync(secretFilePath, req.body.message, 'utf8');
        originalFileName = "titkos_uzenet.txt"; 
    } 
    else {
        fs.unlink(inputPath, () => {});
        return res.status(400).send('Hiányzik a rejtendő adat!');
    }

    const isWindows = process.platform === 'win32';
    const exeName = isWindows ? 'png_readerFINAL.exe' : 'png_readerFINAL';
    const exePath = path.join(__dirname, exeName);

    console.log(`Titkosítás indítása... Fájl neve: ${originalFileName}`);

    execFile(exePath, [inputPath, outputPath, secretFilePath, originalFileName], (error, stdout, stderr) => {
        if (error) {
            console.error("\n[Rendszerhiba]:", error.message);
            
            // Fájlok takarítása hiba esetén
            fs.unlink(inputPath, () => {});
            fs.unlink(secretFilePath, () => {});
            
            // --- ÚJ RÉSZ: Megnézzük, MIT is mondott a C program! ---
            if (stdout && stdout.includes("Nem fer bele")) {
                // Ha a C program szerint túl nagy az adat, ezt üzenjük vissza! (400-as kód)
                return res.status(400).send("❌ A rejtendő adat túl nagy, nem fér bele a választott képbe! Válassz nagyobb sablont, vagy kisebb fájlt.");
            } else if (stdout && stdout.includes("Csak RGB vagy RGBA")) {
                return res.status(400).send("❌ Ez a képformátum nem támogatott (csak RGB/RGBA PNG jó).");
            } else {
                // Minden egyéb, váratlan rendszerhiba
                return res.status(500).send('❌ Ismeretlen hiba történt a titkosítás során a motorban.');
            }
        }

        res.download(outputPath, 'titkos_kep.png', (err) => {
            fs.unlink(inputPath, () => {});
            fs.unlink(outputPath, () => {});
            fs.unlink(secretFilePath, () => {}); 
        });
    });
});


app.listen(port, () => {
    console.log(`\n🚀 A szerver elindult a ${port}-es porton!`);
});