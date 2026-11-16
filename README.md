# Sculpt Live Clean 0.1.0

Effetto VST3 mono/stereo per la pulizia del segnale dal vivo, con tre controlli:
**Denoise, Dereverb, Defeedback**. Interfaccia Sculpt scura, indicatori di ingresso,
uscita e riduzione, trim di uscita e bypass. Prima versione da verificare nel
proprio sistema live prima dell'impiego in spettacolo.

Il progetto privilegia costo di calcolo contenuto e nessuna attesa a blocchi
nel percorso audio. Denoise e riduzione delle code condividono una banca di
12 bande IIR; l'antifeedback usa al massimo sei filtri notch. Non contiene
modelli neurali, convoluzione o oversampling del percorso audio.

## Uso rapido

1. Inserisci il plug-in sul canale microfonico da trattare.
2. Con il microfono aperto e presente soltanto il rumore da ridurre, premi
   **Apprendi rumore** e attendi due secondi. Al termine compare il profilo pronto.
3. Regola Denoise partendo da 25–35%. Il profilo resta fisso: ripeti Learn se
   cambiano microfono, preamplificazione o ambiente.
4. Regola Dereverb con moderazione, ad esempio 15–25% come punto di partenza.
   Confronta tramite Bypass, ascoltando anche le parti più deboli della voce.
5. Per l'antifeedback passa da **Hold** ad **Auto** durante il soundcheck.
   Quando sono state individuate le frequenze problematiche, torna su **Hold**.
   I sei riquadri mostrano le frequenze acquisite. **Reset** cancella questi
   filtri con una transizione graduale.

Queste percentuali sono punti di partenza, non impostazioni universali.
Il preset iniziale è Denoise 25%, Dereverb 15%, Defeedback 50%, output 0 dB,
Hold attivo. Denoise resta trasparente finché non acquisisci un profilo;
Defeedback in Hold con zero frequenze acquisite non applica notch.

## Cosa fanno i tre controlli

### Denoise

Riduce rumore abbastanza stazionario confrontando l'energia corrente delle
bande con il profilo acquisito. Riduzione massima progettata: 12 dB per banda.
I guadagni sono smussati e collegati fra i due canali per mantenere l'immagine
stereo. Gli attacchi riconosciuti vengono protetti.

Non è un separatore di sorgenti: non elimina selettivamente una seconda voce,
il pubblico o uno strumento che si sovrappone al segnale utile. Un profilo
acquisito mentre si parla o si suona può attenuare anche contenuto desiderato.

### Dereverb

Riduce l'energia in decadimento confrontando inviluppi veloci e lenti delle
bande, con protezione degli attacchi. Riduzione massima progettata: 9 dB.
È pensato soprattutto per accorciare code e rendere più pulite le pause.

È una soppressione prudente delle code, non una ricostruzione del suono diretto
sotto riverbero continuo. Può attenuare anche decadimenti musicali voluti:
su chitarra, pianoforte e altre sorgenti con sustain naturale va dosato
ascoltando il risultato. Il nome Dereverb identifica questo comportamento live.

### Defeedback

Rileva picchi stretti persistenti e applica fino a sei notch Q 48, con taglio
massimo di 12 dB per filtro. Intervallo: 120 Hz–12 kHz, ridotto quando richiesto
dal sample rate. I controlli di persistenza e struttura armonica riducono i
falsi riconoscimenti. La prova sintetica a 997 Hz ha richiesto circa 0,3 secondi
per il riconoscimento; non è una garanzia su qualunque innesco reale.

Una nota musicale molto pura e sostenuta può assomigliare a un innesco. Per
questo il flusso previsto è Auto durante il soundcheck e Hold durante lo
spettacolo. Auto può rimanere attivo per acquisire nuovi filtri, con questo
compromesso. Quando tutti i sei posti sono occupati non vengono aggiunti
altri notch; Reset libera la banca. Non viene generato alcun segnale di prova
dal plug-in e non viene alzato automaticamente il guadagno.

La FFT è usata soltanto dall'analizzatore: 2048 punti con avanzamento di 512
campioni e analisi stereo comune. Non trasforma il segnale in uscita e non lo
mette in attesa. In Hold, con Defeedback a zero o con sei filtri occupati, la
FFT viene sospesa. Le analisi delle diverse istanze vengono distribuite nel
tempo per contenere i picchi simultanei di lavoro.

## Latenza e stato

Il plug-in dichiara **0 campioni di latenza aggiunta**: non contiene lookahead
o ritardo audio a blocchi. Restano la latenza di DAW/scheda audio e la risposta
di fase dei filtri IIR. Il tempo necessario per riconoscere un innesco è un
tempo di reazione del rilevatore, distinto dal ritardo del percorso audio.

Il bypass sfuma in 5 ms fino al dry esatto, mantenendo gli stati pronti.
Con tutti i trattamenti a zero e output a 0 dB, il percorso è neutro esatto.

Il progetto DAW salva le manopole, Auto/Hold, bypass, profilo del rumore e
frequenze antifeedback. Le richieste Learn/Reset passano all'audio senza
mutex nel callback. Le frequenze richiamate entrano gradualmente.
Se cambia il sample rate, il vecchio profilo del rumore viene invalidato:
la UI richiede un nuovo Learn. I notch validi vengono mantenuti.

## Compilazione GitHub — file incluso

Il file per avviare la compilazione si trova in:

```
.github/workflows/build-macos-vst3.yml
```

Estrai lo ZIP e carica **il contenuto della cartella SculptLiveClean** nella
radice del repository. Devi vedere subito `CMakeLists.txt`, `Source`, `tools`,
`Tests` e `.github`. Caricare soltanto lo ZIP su GitHub non lo estrae e non
avvia la compilazione.

La cartella `.github` ha il punto iniziale. Se il caricamento dal browser la
omette, usa Add file → Create new file, inserisci il percorso completo sopra
e incolla il contenuto del workflow fornito anche separatamente.

Il workflow parte su ogni push e offre l'avvio manuale **Actions → Build
macOS VST3 → Run workflow**. L'avvio manuale richiede che il file sia presente
nel branch predefinito. Al termine di una compilazione riuscita scarica
**Sculpt-Live-Clean-macOS-VST3**. È disponibile anche il rapporto CPU generato
sul runner Mac nell'artefatto **Sculpt-Live-Clean-verifiche**.

Questo pacchetto contiene sorgenti e configurazione, non un binario macOS
già compilato. Il workflow produce un VST3 universale Apple Silicon/Intel,
con destinazione macOS 11 o successivo, firmato localmente ad hoc e non
notarizzato. Non sono generati AU o AAX.

## Compilazione sul proprio Mac

Servono Xcode Command Line Tools e CMake 3.22 o successivo:

```bash
bash tools/build-macos.sh
```

Risultato: `dist/Sculpt-Live-Clean-macOS-VST3.zip`. La compilazione recupera
JUCE 8.0.4; è possibile indicare una copia locale tramite
`SCULPT_JUCE_SOURCE_DIR=/percorso/JUCE`.

Copia il bundle completo `Sculpt Live Clean.vst3` in:

```
~/Library/Audio/Plug-Ins/VST3/
```

Poi riavvia la DAW o esegui la scansione dei plug-in.

## Test e misura del carico

```bash
bash tools/test-core.sh
bash tools/run-benchmark.sh
```

Il secondo comando genera `build-benchmark/cpu-benchmark.json`. Misura 1, 8
e 16 istanze stereo a 48 kHz / 128 campioni, con profilo acquisito, trattamenti
attivi e notch già inseriti, in Hold e Auto. Include media, 99° percentile,
massimo e tempo CPU del processo. Il budget per un blocco è 2666,67 µs.

Le misure locali di questa consegna provengono da Linux in un ambiente
condiviso e presentano variabilità e picchi oltre il budget con molte istanze.
Non certificano assenza di dropout sul Mac o in una DAW. Il rapporto completo
è conservato in `docs/cpu-benchmark.json`; i risultati del runner Mac e la
prova nel proprio sistema sono necessari per stabilire il numero di istanze
utilizzabile. Non viene nascosto il caso peggiore dietro la sola media.

## Riferimenti e licenze

JUCE 8.0.4 e termini: `ThirdParty/JUCE-8.0.4-LICENSE.md`.
Il nucleo DSP non richiede altre librerie esterne.

Formule biquad, Robert Bristow-Johnson / W3C Audio EQ Cookbook:
https://www.w3.org/TR/audio-eq-cookbook/

Contesto sul compromesso fra riconoscimento di feedback e toni musicali:
https://dbxpro.com/en/product_documents/afs2_manual_5047637-b-pdf
Questo progetto non contiene il codice o l'algoritmo proprietario dbx.

GitHub Actions, esecuzione manuale:
https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow
