package com.couchbase.lite.cpptests;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.TextView;

import java.io.File;

public class MainActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        File path = this.getFilesDir();
        if (!path.exists())
            path.mkdirs();


        String result = runCppTest(path.getPath());

        TextView tv = (TextView) findViewById(R.id.sample_text);
        tv.setText(result);
    }

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    public native String runCppTest(String path);

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("native-lib");
    }
}
