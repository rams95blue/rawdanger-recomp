// Headless wrapper for ps2recomp's ExportPS2Functions.java (used by `rd ghidra`).
// ExportPS2Functions asks for its two output files with Save dialogs, which a
// headless run can't answer. This subclass answers them from script arguments:
//   -postScript RdExportPS2.java <out.toml> <out.csv>
// rd.py copies both scripts into one folder so Ghidra compiles them together.
// @category PS2Recomp

import java.io.File;

public class RdExportPS2 extends ExportPS2Functions {

    @Override
    public File askFile(String title, String approveButtonText) {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("RdExportPS2: expected 2 arguments: <out.toml> <out.csv>");
            return null;
        }
        String t = title.toUpperCase();
        if (t.contains("TOML")) {
            return new File(args[0]);
        }
        if (t.contains("CSV")) {
            return new File(args[1]);
        }
        printerr("RdExportPS2: unexpected prompt '" + title + "'");
        return null;
    }
}
