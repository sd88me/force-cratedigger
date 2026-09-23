/* cratedig_tables.h - Crate Dig (Discogs) filter tables, shared by
 * cratedigger_host.cpp (Force host) and vst/cratedigger_vst.cpp (MPC OS
 * plugin). Moved here verbatim from cratedigger_host.cpp. Plain C data +
 * one lookup, valid C and C++. */
#ifndef CRATEDIG_TABLES_H
#define CRATEDIG_TABLES_H
#include <string.h>

/* ---------------------------------------------------------------------------
 * Crate Dig (Discogs) filter data: genre / style (genre-dependent) /
 * decade / region+country (region narrows country) — for the shadow
 * GUI's FILTERS tab (five `stepper` widgets) and, indirectly, the web
 * GUI (which gets the same lists over a GET endpoint — see server.py).
 *
 * Ported verbatim from the original Move on-device UI's own tables
 * (schwung-webstream's src/ui.js: CRATEDIG_GENRES/CRATEDIG_STYLES/
 * CRATEDIG_DECADES/CRATEDIG_COUNTRIES) — the authoritative real-Discogs-
 * taxonomy source this project already had, not something reinvented
 * here. Every array's index 0 is "" (Discogs field omitted = no filter
 * on that dimension), matching upstream's own "Any" convention.
 *
 * Every value here IS the literal string sent to Discogs (commas,
 * ampersands and all) — there is no separate hand-curated display label
 * per entry (impractical at ~700 style strings total). shadow_font_safe()
 * is called on the value at point of use instead; it already does
 * exactly the uppercase+filter transform needed for Force Shadow's font
 * and is proven correct (see its own comment) — a style string that
 * happens to exceed Force Shadow's `list`/`stepper` on-screen width
 * still filters correctly even if its label is visually truncated.
 * ------------------------------------------------------------------------- */
static const char *CRATEDIG_GENRES[] = {
    "", "Blues", "Brass & Military", "Children's", "Classical", "Electronic",
    "Folk, World, & Country", "Funk / Soul", "Hip Hop", "Jazz", "Latin",
    "Non-Music", "Pop", "Reggae", "Rock", "Stage & Screen",
};
static const int N_CRATEDIG_GENRES = (int)(sizeof(CRATEDIG_GENRES) / sizeof(CRATEDIG_GENRES[0]));

static const char *STYLES_ANY[] = { "" };
static const char *STYLES_BLUES[] = { "", "Boogie Woogie", "Chicago Blues", "Country Blues", "Delta Blues", "East Coast Blues", "Electric Blues", "Harmonica Blues", "Jump Blues", "Louisiana Blues", "Memphis Blues", "Modern Electric Blues", "Piano Blues", "Piedmont Blues", "Rhythm & Blues", "Texas Blues" };
static const char *STYLES_BRASS[] = { "", "Brass Band", "Marches", "Military", "Pipe & Drum" };
static const char *STYLES_CHILDRENS[] = { "", "Educational", "Nursery Rhymes", "Story" };
static const char *STYLES_CLASSICAL[] = { "", "Baroque", "Choral", "Classical", "Contemporary", "Early", "Impressionist", "Medieval", "Modern", "Neo-Classical", "Neo-Romantic", "Opera", "Operetta", "Oratorio", "Post-Modern", "Renaissance", "Romantic", "Serial", "Twelve-tone", "Zarzuela" };
static const char *STYLES_ELECTRONIC[] = { "", "Abstract", "Acid", "Acid House", "Acid Jazz", "Ambient", "Ballroom", "Baltimore Club", "Bassline", "Beatdown", "Berlin-School", "Big Beat", "Breakbeat", "Breakcore", "Breaks", "Broken Beat", "Chillwave", "Chiptune", "Dance-pop", "Dark Ambient", "Darkwave", "Deep House", "Deep Techno", "Disco", "Disco Polo", "Donk", "Doomcore", "Downtempo", "Drone", "Drum n Bass", "Dub", "Dub Techno", "Dubstep", "Dungeon Synth", "EBM", "Electro", "Electro House", "Electroclash", "Euro House", "Euro-Disco", "Eurobeat", "Eurodance", "Experimental", "Freestyle", "Funkot", "Future Jazz", "Gabber", "Garage House", "Ghetto", "Ghetto House", "Ghettotech", "Glitch", "Goa Trance", "Grime", "Hands Up", "Happy Hardcore", "Hard Beat", "Hard House", "Hard Techno", "Hard Trance", "Hardcore", "Hardstyle", "Harsh Noise Wall", "Hi NRG", "Hip Hop", "Hip-House", "House", "IDM", "Illbient", "Industrial", "Italo House", "Italo-Disco", "Italodance", "J-Core", "Jazzdance", "Juke", "Jumpstyle", "Jungle", "Latin", "Leftfield", "Lento Violento", "Makina", "Minimal", "Minimal Techno", "Modern Classical", "Musique Concr\xc3\xa8te", "Neo Trance", "Neofolk", "Nerdcore Techno", "New Age", "New Beat", "New Wave", "Noise", "Nu-Disco", "Power Electronics", "Progressive Breaks", "Progressive House", "Progressive Trance", "Psy-Trance", "Rhythmic Noise", "Schranz", "Skweee", "Sound Collage", "Speed Garage", "Speedcore", "Synth-pop", "Synthwave", "Tech House", "Tech Trance", "Techno", "Trance", "Tribal", "Tribal House", "Trip Hop", "Tropical House", "UK Funky", "UK Garage", "Vaporwave", "Witch House" };
static const char *STYLES_FOLK[] = { "", "Aboriginal", "African", "Andalusian Classical", "Appalachian Music", "Bangladeshi Classical", "Basque Music", "Bengali Music", "Bhangra", "Bluegrass", "Cajun", "Cambodian Classical", "Canzone Napoletana", "Carnatic", "Catalan Music", "Celtic", "Chacarera", "Chamam\xc3\xa9", "Chinese Classical", "Chutney", "Cobla", "Copla", "Country", "Dangdut", "\xc3\x89ntekhno", "Fado", "Filk", "Flamenco", "Folk", "Funan\xc3\xa1", "Gagaku", "Gamelan", "Gospel", "Griot", "Guarania", "Hawaiian", "Highlife", "Hillbilly", "Hindustani", "Honky Tonk", "Indian Classical", "Jota", "Kaseko", "Keroncong", "Kizomba", "Klasik", "Klezmer", "Korean Court Music", "La\xc3\xafk\xc3\xb3", "Lao Music", "Liscio", "Luk Krung", "Luk Thung", "Maloya", "Mbalax", "Min'y\xc5\x8d", "Mizrahi", "Mouth Music", "Mugham", "N\xc3\xa9pzene", "Nordic", "Ottoman Classical", "Overtone Singing", "Pacific", "Pasodoble", "Persian Classical", "Philippine Classical", "Phleng Phuea Chiwit", "Piobaireachd", "Polka", "Progressive Bluegrass", "Ra\xc3\xaf", "Rebetiko", "Romani", "Rune Singing", "Salegy", "S\xc3\xa1mi Music", "Sea Shanties", "S\xc3\xa9ga", "Sephardic", "Soukous", "Thai Classical", "Volksmusik", "Waiata", "Western Swing", "Yemenite Jewish", "Zamba", "Zemer Ivri", "Zouk", "Zydeco" };
static const char *STYLES_FUNK[] = { "", "Afrobeat", "Bayou Funk", "Boogie", "Contemporary R&B", "Disco", "Free Funk", "Funk", "Gogo", "Gospel", "Minneapolis Sound", "Neo Soul", "New Jack Swing", "P.Funk", "Psychedelic", "Rhythm & Blues", "Soul", "Swingbeat", "UK Street Soul" };
static const char *STYLES_HIPHOP[] = { "", "Bass Music", "Beatbox", "Bongo Flava", "Boom Bap", "Bounce", "Britcore", "Cloud Rap", "Conscious", "Crunk", "Cut-up/DJ", "DJ Battle Tool", "Electro", "Favela Funk", "G-Funk", "Gangsta", "Go-Go", "Grime", "Hardcore Hip-Hop", "Hiplife", "Horrorcore", "Hyphy", "Instrumental", "Jazzy Hip-Hop", "Kwaito", "Miami Bass", "Motswako", "Pop Rap", "Ragga HipHop", "RnB/Swing", "Screw", "Spaza", "Thug Rap", "Trap", "Trip Hop", "Turntablism" };
static const char *STYLES_JAZZ[] = { "", "Afro-Cuban Jazz", "Afrobeat", "Avant-garde Jazz", "Big Band", "Bop", "Bossa Nova", "Cape Jazz", "Contemporary Jazz", "Cool Jazz", "Dixieland", "Easy Listening", "Free Improvisation", "Free Jazz", "Fusion", "Gypsy Jazz", "Hard Bop", "Jazz-Funk", "Jazz-Rock", "Latin Jazz", "Modal", "Post Bop", "Ragtime", "Smooth Jazz", "Soul-Jazz", "Space-Age", "Swing" };
static const char *STYLES_LATIN[] = { "", "Afro-Cuban", "Ax\xc3\xa9", "Bachata", "Ba\xc3\xa3o", "Batucada", "Beguine", "Bolero", "Bomba", "Boogaloo", "Bossanova", "Candombe", "Carimb\xc3\xb3", "Cha-Cha", "Champeta", "Charanga", "Choro", "Compas", "Conjunto", "Corrido", "Cuatro", "Cubano", "Cumbia", "Danzon", "Descarga", "Forr\xc3\xb3", "Gaita", "Guaguanc\xc3\xb3", "Guajira", "Guaracha", "Jibaro", "Joropo", "Lambada", "Mambo", "Marcha Carnavalesca", "Mariachi", "Marimba", "Merengue", "MPB", "Musette", "M\xc3\xbasica Criolla", "Norte\xc3\xb1o", "Nueva Cancion", "Nueva Trova", "Occitan", "Pachanga", "Plena", "Porro", "Quechua", "Ranchera", "Reggaeton", "Rumba", "Salsa", "Samba", "Samba-Can\xc3\xa7\xc3\xa3o", "Seresta", "Son", "Son Montuno", "Sonero", "Tango", "Tejano", "Timba", "Trova", "Vallenato" };
static const char *STYLES_NONMUSIC[] = { "", "Audiobook", "Comedy", "Dialogue", "Education", "Field Recording", "Health-Fitness", "Interview", "Monolog", "Movie Effects", "Poetry", "Political", "Promotional", "Public Broadcast", "Public Service Announcement", "Radioplay", "Religious", "Sermon", "Sound Art", "Sound Poetry", "Special Effects", "Speech", "Spoken Word", "Technical", "Therapy" };
static const char *STYLES_POP[] = { "", "Ballad", "Barbershop", "Bollywood", "Break-In", "Bubblegum", "Chanson", "Enka", "Ethno-pop", "Europop", "Indie Pop", "J-pop", "K-pop", "Karaoke", "Kay\xc5\x8dkyoku", "Levenslied", "Light Music", "Music Hall", "N\xc3\xa9o Kyma", "Novelty", "Parody", "Schlager", "Vocal" };
static const char *STYLES_REGGAE[] = { "", "Azonto", "Bubbling", "Calypso", "Dancehall", "Dub", "Dub Poetry", "Junkanoo", "Lovers Rock", "Mento", "Ragga", "Rapso", "Reggae", "Reggae Gospel", "Reggae-Pop", "Rocksteady", "Roots Reggae", "Ska", "Soca", "Steel Band" };
static const char *STYLES_ROCK[] = { "", "Acid Rock", "Acoustic", "Alternative Rock", "AOR", "Arena Rock", "Art Rock", "Atmospheric Black Metal", "Avantgarde", "Beat", "Black Metal", "Blues Rock", "Brit Pop", "Classic Rock", "Coldwave", "Country Rock", "Crust", "Death Metal", "Deathcore", "Deathrock", "Depressive Black Metal", "Doo Wop", "Doom Metal", "Dream Pop", "Emo", "Ethereal", "Experimental", "Folk Metal", "Folk Rock", "Funeral Doom Metal", "Funk Metal", "Garage Rock", "Glam", "Goregrind", "Goth Rock", "Gothic Metal", "Grindcore", "Grunge", "Hard Rock", "Hardcore", "Heavy Metal", "Horror Rock", "Indie Rock", "Industrial", "Krautrock", "Lo-Fi", "Lounge", "Math Rock", "Melodic Death Metal", "Melodic Hardcore", "Metalcore", "Mod", "NDW", "Neofolk", "New Wave", "No Wave", "Noise", "Noisecore", "Nu Metal", "Oi", "Parody", "Pop Punk", "Pop Rock", "Pornogrind", "Post Rock", "Post-Hardcore", "Post-Metal", "Post-Punk", "Power Metal", "Power Pop", "Power Violence", "Prog Rock", "Progressive Metal", "Psychedelic Rock", "Psychobilly", "Pub Rock", "Punk", "Rock & Roll", "Rock Opera", "Rockabilly", "Shoegaze", "Ska", "Skiffle", "Sludge Metal", "Soft Rock", "Southern Rock", "Space Rock", "Speed Metal", "Stoner Rock", "Surf", "Swamp Pop", "Symphonic Rock", "Technical Death Metal", "Thrash", "Twist", "Viking Metal", "Y\xc3\xa9-Y\xc3\xa9" };
static const char *STYLES_STAGE[] = { "", "Musical", "Score", "Soundtrack", "Theme" };

typedef struct GenreStyles { const char *genre; const char **styles; int n; } GenreStyles;
#define GS(arr) (arr), (int)(sizeof(arr)/sizeof(arr[0]))
static const GenreStyles CRATEDIG_STYLES[] = {
    { "",                        GS(STYLES_ANY) },
    { "Blues",                   GS(STYLES_BLUES) },
    { "Brass & Military",        GS(STYLES_BRASS) },
    { "Children's",              GS(STYLES_CHILDRENS) },
    { "Classical",                GS(STYLES_CLASSICAL) },
    { "Electronic",               GS(STYLES_ELECTRONIC) },
    { "Folk, World, & Country",   GS(STYLES_FOLK) },
    { "Funk / Soul",              GS(STYLES_FUNK) },
    { "Hip Hop",                  GS(STYLES_HIPHOP) },
    { "Jazz",                     GS(STYLES_JAZZ) },
    { "Latin",                    GS(STYLES_LATIN) },
    { "Non-Music",                GS(STYLES_NONMUSIC) },
    { "Pop",                      GS(STYLES_POP) },
    { "Reggae",                   GS(STYLES_REGGAE) },
    { "Rock",                     GS(STYLES_ROCK) },
    { "Stage & Screen",           GS(STYLES_STAGE) },
};
#undef GS
static const int N_CRATEDIG_STYLE_GENRES = (int)(sizeof(CRATEDIG_STYLES) / sizeof(CRATEDIG_STYLES[0]));

/* Looks up the style array for whatever genre is currently selected
 * (matched by value string, not index — CRATEDIG_STYLES and
 * CRATEDIG_GENRES are independent arrays kept in the same order by
 * hand, so matching by string is a hair more robust than trusting that
 * ordering never drifts). Falls back to STYLES_ANY (just "") if the
 * current genre has no entry (shouldn't happen — every real genre does). */
static const GenreStyles *styles_for_genre(const char *genre_value) {
    for (int i = 0; i < N_CRATEDIG_STYLE_GENRES; i++)
        if (!strcmp(CRATEDIG_STYLES[i].genre, genre_value)) return &CRATEDIG_STYLES[i];
    return &CRATEDIG_STYLES[0];
}

static const char *CRATEDIG_DECADES[] = {
    "", "1950s", "1960s", "1970s", "1980s", "1990s", "2000s", "2010s", "2020s",
};
static const int N_CRATEDIG_DECADES = (int)(sizeof(CRATEDIG_DECADES) / sizeof(CRATEDIG_DECADES[0]));

/* Region is a UI-only grouping (not a real Discogs API field — the API
 * only takes `country`); it exists purely so the shadow GUI's country
 * stepper doesn't have to cycle through ~70 countries one at a time to
 * reach, say, Japan. Selecting a region resets the country index to 0
 * ("Any" within that region) the same way selecting a genre resets style. */
static const char *CRATEDIG_REGIONS[] = { "Any", "Americas", "Europe", "Africa", "Asia", "Oceania" };
static const int N_CRATEDIG_REGIONS = (int)(sizeof(CRATEDIG_REGIONS) / sizeof(CRATEDIG_REGIONS[0]));

static const char *COUNTRIES_ANY[] = { "" };
static const char *COUNTRIES_AMERICAS[] = { "", "Argentina", "Brazil", "Canada", "Chile", "Colombia", "Cuba", "Haiti", "Jamaica", "Mexico", "Peru", "Puerto Rico", "Trinidad & Tobago", "US", "Venezuela" };
static const char *COUNTRIES_EUROPE[] = { "", "Austria", "Belgium", "Bulgaria", "Croatia", "Czech Republic", "Denmark", "Finland", "France", "Germany", "Greece", "Hungary", "Iceland", "Ireland", "Italy", "Netherlands", "Norway", "Poland", "Portugal", "Romania", "Russia", "Serbia", "Spain", "Sweden", "Switzerland", "Turkey", "UK", "Ukraine" };
static const char *COUNTRIES_AFRICA[] = { "", "Algeria", "Benin", "Cameroon", "Cape Verde", "Congo", "Egypt", "Ethiopia", "Ghana", "Guinea", "Ivory Coast", "Kenya", "Mali", "Morocco", "Nigeria", "Senegal", "South Africa", "Tanzania", "Zimbabwe" };
static const char *COUNTRIES_ASIA[] = { "", "China", "India", "Indonesia", "Iran", "Israel", "Japan", "Lebanon", "Pakistan", "Philippines", "South Korea", "Taiwan", "Thailand", "Vietnam" };
static const char *COUNTRIES_OCEANIA[] = { "", "Australia", "New Zealand" };
static const char **CRATEDIG_COUNTRIES_BY_REGION[] = {
    COUNTRIES_ANY, COUNTRIES_AMERICAS, COUNTRIES_EUROPE, COUNTRIES_AFRICA, COUNTRIES_ASIA, COUNTRIES_OCEANIA,
};
static const int N_COUNTRIES_BY_REGION[] = {
    (int)(sizeof(COUNTRIES_ANY)/sizeof(COUNTRIES_ANY[0])),
    (int)(sizeof(COUNTRIES_AMERICAS)/sizeof(COUNTRIES_AMERICAS[0])),
    (int)(sizeof(COUNTRIES_EUROPE)/sizeof(COUNTRIES_EUROPE[0])),
    (int)(sizeof(COUNTRIES_AFRICA)/sizeof(COUNTRIES_AFRICA[0])),
    (int)(sizeof(COUNTRIES_ASIA)/sizeof(COUNTRIES_ASIA[0])),
    (int)(sizeof(COUNTRIES_OCEANIA)/sizeof(COUNTRIES_OCEANIA[0])),
};

#endif
