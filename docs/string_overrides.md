# String overrides

Place a matching JSON5 sidecar in the VFS data search path, for example
`data/overrides/estrings.json5` or `data/mods/my-mod/data/estrings.json5`:

```json5
{
    // String IDs start at 1; omitted strings keep their LBX value.
    1: "foobar",
    '2': 'Another string',
}
```

The sidecar replaces `.lbx` with `.json5`, retaining any directory component.
The actual English H-string filename is `hestrngs.lbx`, so its sidecar is
`hestrngs.json5` (not `hestrings.json5`). Localized LBX filenames have their own
matching sidecars. Files containing several languages use logical string IDs
independent of the selected language.

Standard VFS precedence applies, including loose overrides and mounted packages.
The highest-priority sidecar wins as a whole file; sidecars are not merged across
packages. The LBX remains required. Each helper access reads the currently resolved
sidecar and compares its bytes with the cached source. Unchanged files reuse their
parsed overrides; changed files are parsed and converted again. Added and removed
files, same-size edits, and changes in VFS precedence are detected without a cache
reset. Invalid replacements report an error rather than using stale cached text.

This caches parsing and encoding work, not file I/O: each access still performs
VFS lookup, reading (and ZIP decompression where applicable), and byte comparison.
Loops calling `Apply_String` check the sidecar for each string. The cache retains
both source bytes and parsed strings. `strings::Load_Strings()` still clears it
to release storage during full resource reloads.

Changes take effect when the corresponding strings are next loaded, not when
already-resident strings are drawn. Packed tables still need their normal reload.
ZIP changes are visible only when VFS exposes the updated archive contents; this
does not add archive watching or remounting.

JSON5 comments, single quotes, and trailing commas are supported. Bare decimal
keys are an intentional extension to JSON5, which normally requires numeric keys
to be quoted. IDs must be positive decimal integers and values must be strings;
duplicate IDs are errors. Unused IDs do not add strings and have no effect.
Files are limited to 4 MiB. Text is UTF-8 and converted to the game's CP850
encoding; unrepresentable characters and embedded NUL are errors.

Packed tables are rebuilt in temporary growable storage before pointers are
published. Their final size can exceed both the LBX entry size and the old static
buffer size. E/H/Russ own growable persistent buffers; H offsets use `size_t`.
Pool-backed tables allocate a block for the final size, and the exclusively owned
technology/special-name pools grow when necessary. Shared screen pools retain
their overall capacity and normal block lifetime; individual blocks are never
extended over neighboring allocations. Reloading rebuilds all table pointers.

Single-string copies into fixed destination fields still require the replacement
to fit that field, including its terminator. Values are not silently truncated.
Packed tables whose legacy parsers skip NUL padding (technology and race tables) cannot contain empty
replacement strings. Preserve format placeholders and game control codes:
overrides do not validate `printf` arguments or message-token semantics.

## Active plain-text resources and loaders

All numbers below are sidecar IDs, starting at 1. Source indices start at 0.

| LBX files | Loader | Numbering |
| --- | --- | --- |
| `estrings`, `estrGERM`, `estrFREN`, `estrSPAN`, `estrITAL`, `estrPOLI` | `ESTRINGS::Load_E_Strings` | 1-812, packed table order. Slot 369 remains the loader-selected main-text filename. |
| `HESTRNGS`, `HGSTRNGS`, `HFSTRNGS`, `HSSTRNGS`, `HISTRNGS` | `HAROLD::Load_H_Strings`; `H_Message`, also used by `MISC::Get_LBX_Message` | 1-397, packed table order. |
| `rstring0` through `rstring5` | `RUSS::Load_Russ_Strings`, then COLCALC/COMBFIND/MSG/NETMOX/NETSTART/NETTURN/REPORT parsers | 1-177 across the complete packed table. |
| `billtext`, `billtex2`, `jimtext`, `jimtext2`, `KENTEXT`, `KENTEXT1` | `JIM::Get_Text_Message`; KEN delegates to JIM | Logical message index + 1, not physical entry + 1. Six language entries share an ID. KENTEXT1 starts at 1 for KEN message index 80. |
| `EVENTMSE`, `EVENTMSG`, `EVENTMSF`, `EVENTMSS`, `EVENTMSI`, `EVENTMSP` | `EVENTS::Get_Event_Message` | `event_type * 4 + event_subtype + 1`. |
| `antarmsg` | `ANTAROOM::Get_Antaran_Room_Message` | 1-8: two message sets, four alternatives each. |
| `councmsg` | `council::Get_Player_Voted_Winner_Message` | 1-14, excluding the language block offset. |
| `maintext`, `maingerm`, `mainfren`, `mainspan`, `mainital`, `mainpoli` | `MAINPUPS::System_Special_Desc` | Description entry + 1. |
| `techname` | `TECHINIT::Load_Tech_Names` | Packed table order within the selected language. Existing explicit configuration name overrides still take precedence. |
| `RACESTUF` | `raceopt::Reload_Option_Strings`, `LOADER::Load_Player_Special_Names` | 1-64 option strings; 65-96 special-ability names. Binary trait entries are untouched. Dynamically generated primary-pick labels still replace their corresponding LBX labels. |
| `SKILDESC`, `GERSKLLS`, `FRESKLLS`, `SPASKLLS`, `ITASKLLS` | `OFFICER::Load_Officer_Skill_Name_Descriptions` | 1-27 skill names; 28-54 descriptions. |
| `TECHDESC`, `GERTECD`, `FRETECD`, `SPATECD`, `ITATECD` | `DESIGN::Load_Design_Screen_Pictures` | 1-39 special descriptions (entry 1); 40-79 weapon notes (entry 3). |
| `shipname` | `DESIGN::Get_Ship_Design_Name` | Record index + 1. |
| `RACENAME` | `RACESEL::Naming_Popup`, `INITGAME::Init_Players` | Record index + 1, eight candidate names per race. |
| `starname` | `HOMEGEN::Assign_Home_Star_Names`, `MAPGEN::Universe_Generation` | 1-13 home-system names; 14-842 random star names. |
| `CREDITS`, `GERCRDTS`, `FRECRDTS`, `SPACRDTS`, `ITACRDTS` | `MAINMENU::Load_Credits` | Display lines in order, two per credit. The leading record containing the credit count is not overridable. |

## Other string-bearing resources identified

These are mixed binary records, not plain string tables, and are deliberately
not enabled by this implementation:

- `HELP`, `GER_HELP`, `FRE_HELP`, `SPA_HELP`, `ITA_HELP`: title/body, animation
  references, and linked records. Loaders are in TEXTBOX, INFO, EVANHELP, TECH,
  COLBLDG, DESBOX, and FLT2.
- `DIPLOMSE`, `DIPLOMSG`, `DIPLOMSF`, `DIPLOMSS`, `DIPLOMSI`, `DIPLOMSP`:
  `DIP_SCRN::Get_Diplomacy_Statement` reads metadata and alternative statements.
- `HERODATA`, `HERODATG`, `HERODATF`, `HERODATS`, `HERODATI`:
  `INITGAME::Init_Leaders` and `HAROLD::Load_Leader_Titles` read names/titles
  embedded in leader data.

The catalog also lists `ENGMSG`, `MSGENG`, and `CUSTMSTR`, but no active runtime
loaders were found. Catalog classification alone is not enough to enable overrides.

## Implementation boundary

`farload` and VFS remain generic binary readers. String-owning loaders explicitly
call `string_overrides::Apply_String`, `Apply_Table`, `Load_Message`,
`Load_Raw_Table`, or `Reload_Raw_Table`, supplying their own logical IDs, counts,
and storage ownership. `Apply_Table` accepts an owned vector, not a fixed buffer.
The shared helper contains no resource filename registry or game record layouts.
The `Try_Apply_*` APIs return status and a diagnostic without modifying the
destination on failure; runtime wrappers use the existing fatal-error API.
No exceptions are introduced.