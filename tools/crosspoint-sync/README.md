# crosspoint-sync

Terminal note sync for CrossPoint Reader.

```bash
./crosspoint-sync
./crosspoint-sync "~/Documents/My Notes"
./crosspoint-sync --url http://192.168.1.42 "~/Documents/My Notes" "/notes"
```

The tool lists notes from CrossPoint over WebDAV, shows a Bubble Tea checklist, copies selected notes one way to the local folder, asks before overwriting local files, and can delete successfully copied notes from CrossPoint.

Build a reusable binary:

```bash
cd tools/crosspoint-sync
go build -o crosspoint-sync .
```
