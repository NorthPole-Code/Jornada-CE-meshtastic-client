# Jornada-CE-meshtastic-client
Native meshtastic client for HP Jornada 720/728 Win CE. Used in conjunction with a meshtastic node, connected over serial. 

MESHTASTIC for HP Jornada 720 / 728
Help / README
Version 1.4
Made by NorthPolePoint
05.Jan.2026

Tested with:
Lilygo T-Beam running Meshtastic firmware 2.7.15

------------------------------------------------------------

1. OVERVIEW

This application is a native Windows CE (HPC2000) Meshtastic client designed for:

- HP Jornada 720
- HP Jornada 728

The program communicates with a Meshtastic device via serial connection and supports:

- Node display
- Message history
- Offline raster maps

------------------------------------------------------------

2. REQUIRED FOLDER STRUCTURE

All application data must exist inside:

\Storage Card\Meshtastic\

Example structure:

\Storage Card\
 └── Meshtastic\
      ├── Maps\
      ├── config.cfg      (auto-created by app when settings saved)
      └── Messages\      (auto-created)


------------------------------------------------------------

3. MAPS FOLDER

Location:

\Storage Card\Meshtastic\Maps\

------------------------------------------------------------

3.1 MAP FILE FORMAT (.BMP + .BPW)

Each map consists of TWO files:

1) Bitmap image:
   .bmp

2) World file:
   .bpw

Both files MUST have identical base filenames.

Examples (valid pairs):

Coast.bmp
Coast.bpw

White_Forest.bmp
White_Forest.bpw

The application pairs maps strictly by filename: for every <name>.bmp there must be a matching <name>.bpw.

------------------------------------------------------------

3.2 DEFAULT MAP NAME

The application looks for a map named:

map.bmp
map.bpw

Recommended default setup:

\Maps\
 ├── map.bmp
 ├── map.bpw

------------------------------------------------------------

4. CONFIG FILE (AUTO-CREATED by app)

Location:

\Storage Card\Meshtastic\config.cfg

Created automatically on first run.

Stores:

- Serial port configuration
- Application settings
- UI preferences
- Runtime configuration data

Do not modify manually unless necessary.

------------------------------------------------------------

5. MESSAGES FOLDER (AUTO-CREATED)

Location:

\Storage Card\Meshtastic\Messages\

Created automatically on first run.

Contains:

- Message history files
- Stored in plain text format
- Human-readable
- Can be opened with any text editor

No database is used.

This allows easy backup and inspection.

------------------------------------------------------------

6. WIRING AND SERIAL CONNECTION

This application communicates with a Meshtastic node over a serial connection.

Tested hardware:
- Lilygo T-Beam
- TTL-to-Serial (RS232) converter
- HP Jornada sync cable

------------------------------------------------------------

6.1 TTL CONNECTION (Meshtastic Node to TTL Adapter)

Typical Meshtastic boards (e.g., T-Beam) expose UART pins:

- TX
- RX
- GND
- 3.3V

Connect as follows (standard configuration):

Meshtastic TX  →  TTL Adapter RX
Meshtastic RX  →  TTL Adapter TX
Meshtastic GND →  TTL Adapter GND

IMPORTANT:

• TX and RX are normally crossed.
• The TTL/serial adapter must operate at 3.3V logic level.
• 3.3V power for the TTL adapter can typically be provided directly from the Meshtastic node (if supported).
• Do NOT use 5V unless you are absolutely sure the hardware supports it.

NOTE:

Some boards label RX/TX from their own perspective, while others label from the opposite side of the connection. If communication does not work:

- Try crossed wiring (TX→RX, RX→TX)
- Try non-crossed wiring (TX→TX, RX→RX)

Some adapters have swapped or misleading labels.

------------------------------------------------------------

6.2 TTL TO RS232 CONVERTER

The TTL adapter must convert 3.3V UART levels to true RS232 levels.

Examples:
- MAX3232-based adapters
- USB-TTL + RS232 level shifter
- Dedicated TTL-to-DB9 serial boards

RECOMMENDATION:
Use a TTL-to-RS232 adapter with a MALE DB9 connector.

Reason:
The HP Jornada sync cable is directly compatible with a male DB9 connector, which simplifies wiring and often avoids the need for additional gender changers.

------------------------------------------------------------

6.3 CONNECTING TO THE JORNADA

The HP Jornada 720/728 uses a serial sync cable.

Connection chain:

Meshtastic Node
   ↓
TTL (3.3V UART)
   ↓
TTL-to-RS232 Converter (DB9 male recommended)
   ↓
Jornada Sync Cable
   ↓
HP Jornada

------------------------------------------------------------

6.4 NULL MODEM ADAPTER NOTE

Whether a null modem adapter is required depends on your DB9 connector type and internal wiring.

Case 1:
TTL-to-RS232 adapter with MALE DB9 connector:
→ In testing, a null modem adapter was NOT required.

Case 2:
TTL-to-RS232 adapter with FEMALE DB9 connector:
→ A null modem adapter WAS required.

Reason:
Some adapters are wired as DTE, others as DCE.
If TX/RX are not properly crossed at RS232 level, communication will fail.

If no data is received:
- Try adding or removing a null modem adapter.
- Verify TX/RX crossover.
- Verify common ground.
- Test both crossed and non-crossed TTL wiring.

------------------------------------------------------------

7. CONNECTING IN THE APP

After all wiring is complete and the Meshtastic node is powered on:

1) Start the application on the Jornada.
2) Open the main screen (or serial screen, depending on version).
3) Tap the "Connect" button.

This opens the serial port and the app starts communicating with the Meshtastic node.

------------------------------------------------------------

7.1 INITIAL DOWNLOAD (HOURGLASS INDICATOR)

After connecting, the app performs an initial download over serial, including:

- Node settings
- Node list (known nodes)

During this phase, an hourglass animation is shown.


------------------------------------------------------------

8. CREATING MAPS USING QGIS (OpenStreetMap, EPSG:4326)

Maps must be created using QGIS and exported as:

- .bmp (image)
- .bpw (world file)

Projection must be:

EPSG:4326 (WGS84)

Maximum export resolution:

1500 x 1500 pixels

Larger images may cause memory issues on HP Jornada devices.

------------------------------------------------------------

8.1 INSTALL QGIS

Download QGIS from:
https://qgis.org

Any recent 3.x version should work.

------------------------------------------------------------

8.2 CREATE NEW PROJECT (SET CRS)

1) Open QGIS
2) Create a new project
3) Set Project CRS to:

EPSG:4326 – WGS 84

To set CRS:
- Click the CRS indicator (bottom right)
- Search for: 4326
- Select: WGS 84 (EPSG:4326)

------------------------------------------------------------

8.3 ADD OPENSTREETMAP (XYZ TILES)

1) Open the Browser panel
2) Right click "XYZ Tiles"
3) Choose "New Connection"
4) Name: OpenStreetMap
5) URL:
   https://tile.openstreetmap.org/{z}/{x}/{y}.png
6) Add the layer to the map

------------------------------------------------------------

8.4 EXPORT MAP AS BMP + BPW

In QGIS:

Project → Import/Export → Export Map to Image

Settings:
- Format: BMP
- Width/Height: set manually
- Maximum: 1500 x 1500 pixels (do NOT exceed)

Enable:
- Generate world file  (creates .bpw)

After export you should have BOTH files, for example:
- map.bmp
- map.bpw

Copy the files to:
\Storage Card\Meshtastic\Maps\

------------------------------------------------------------

END OF DOCUMENT
