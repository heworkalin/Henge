package dev.henge.driverplugin;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * This APK carries no UI of its own — it exists so the host can read its
 * manifest metadata and its native library directory. The activity is here only
 * so the package is launchable and the values can be checked by hand after
 * install.
 */
public class MainActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        StringBuilder sb = new StringBuilder();
        sb.append("Vulkan driver plugin\n\n");
        try {
            Bundle meta = getPackageManager()
                    .getApplicationInfo(getPackageName(), android.content.pm.PackageManager.GET_META_DATA)
                    .metaData;
            sb.append("fclPlugin  : ").append(meta.getBoolean("fclPlugin")).append('\n');
            sb.append("driver     : ").append(meta.getString("driver")).append('\n');
            sb.append("driver_lib : ").append(meta.getString("driver_lib")).append('\n');
            sb.append("driver_gpu : ").append(meta.getString("driver_gpu")).append('\n');
        } catch (Exception e) {
            sb.append("metadata unreadable: ").append(e).append('\n');
        }
        sb.append("\nnativeLibraryDir:\n  ").append(getApplicationInfo().nativeLibraryDir).append('\n');
        sb.append("\nvalue of the extracted library:\n");
        java.io.File dir = new java.io.File(getApplicationInfo().nativeLibraryDir);
        String[] names = dir.list();
        if (names == null || names.length == 0) {
            sb.append("  (empty — the driver is not packaged)\n");
        } else {
            for (String n : names) sb.append("  ").append(n).append('\n');
        }

        TextView view = new TextView(this);
        view.setText(sb.toString());
        view.setTextSize(12f);
        view.setTypeface(Typeface.MONOSPACE);
        view.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(view);
        setContentView(scroll);
    }
}
