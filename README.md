**mvp**  
mvp is a small Linux command-line utility that behaves like mv just with the addition of a progress bar.  
It is meant for the common cases where mv is exactly what you want, but you also want visible feedback for large moves across filesystems.  
**Features**  
- Fast-path moves with rename(2) when the source and destination are on the same filesystem.  
- Progress bar on stderr when moving regular files or directory trees across filesystems.  
- Recursive directory moves.  
- Temporary staging near the destination so incomplete cross-filesystem moves do not land at the final path.  
**Build**  
make  
   
**Install**  
sudo make install  
   
By default this installs mvp to /usr/local/bin/mvp.  
**Usage**  
mvp SOURCE DEST  
 mvp SOURCE... DIRECTORY  
   
**How It Works**  
If Linux can satisfy the move with a plain rename, mvp does that immediately, just like mv.  
If the move crosses filesystems, mvp falls back to copying the data, shows a progress bar while bytes are being transferred, then removes the original source after the copy completes successfully.  
**Scope**  
mvp focuses on the standard everyday move workflow and currently supports:  
- Regular files  
- Directories  
- Symlinks  
- FIFOs and common special device nodes  
It intentionally keeps the interface small and does not yet aim to replicate every GNU mv flag.  
