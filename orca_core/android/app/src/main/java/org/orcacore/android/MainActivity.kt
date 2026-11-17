package org.orcacore.android

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Process
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.io.FileOutputStream
import javax.microedition.khronos.egl.EGL10
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.egl.EGLContext
import javax.microedition.khronos.egl.EGLDisplay
import javax.microedition.khronos.opengles.GL10

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var glView: GLSurfaceView
    private var padButtons = 0
    private var stickX = 128
    private var stickY = 128
    private var cStickX = 128
    private var cStickY = 128
    private var triggerL = 0
    private var triggerR = 0
    @Volatile private var audioRunning = false
    @Volatile private var audioTrack: AudioTrack? = null
    private var audioThread: Thread? = null
    @Volatile private var rumbleRunning = false
    private var rumbleThread: Thread? = null
    private var rumbleState = -1
    private val vibrator by lazy { getSystemService(Vibrator::class.java) }
    private var pendingConsole = 0 // 0 = GameCube, 1 = Wii
    private var pendingDiscUri: Uri? = null

    private external fun nativeInit(): String
    private external fun nativeLoadDisc(path: String, consoleKind: Int, keyText: String): String
    private external fun nativeSetPadState(buttons: Int, sx: Int, sy: Int, csx: Int, csy: Int, tl: Int, tr: Int)
    private external fun nativeStatus(): String
    private external fun nativeRumbleState(): Int
    private external fun nativeAudioRate(): Int
    private external fun nativeReadAudio(buffer: ShortArray): Int
    private external fun nativeDrawFrame()
    private external fun nativeShutdown()

    companion object {
        init { System.loadLibrary("orca_android") }
        private const val OPEN_DISC_REQUEST = 4107
        private const val OPEN_KEYS_REQUEST = 4108
        private const val EGL_OPENGL_ES3_BIT_KHR = 0x0040
        private const val EGL_CONTEXT_MAJOR_VERSION_KHR = 0x3098
        private const val EGL_CONTEXT_MINOR_VERSION_KHR = 0x30FB
        private const val PAD_A = 0x0100
        private const val PAD_B = 0x0200
        private const val PAD_X = 0x0400
        private const val PAD_Y = 0x0800
        private const val PAD_START = 0x1000
        private const val PAD_LEFT = 0x0001
        private const val PAD_RIGHT = 0x0002
        private const val PAD_DOWN = 0x0004
        private const val PAD_UP = 0x0008
    }

    private fun createAudioTrack(rate: Int): AudioTrack? {
        val minBytes = AudioTrack.getMinBufferSize(
            rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT
        )
        if (minBytes <= 0) return null
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(rate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build()
            )
            .setBufferSizeInBytes(maxOf(minBytes, rate / 5 * 4))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        if (track.state != AudioTrack.STATE_INITIALIZED) {
            track.release()
            return null
        }
        return track
    }

    private fun startAudio() {
        stopAudio()
        val initialRate = nativeAudioRate()
        val initialTrack = createAudioTrack(initialRate)
        if (initialTrack == null) {
            status.text = "Audio Android: no se pudo crear AudioTrack a $initialRate Hz."
            return
        }
        audioTrack = initialTrack
        audioRunning = true
        audioThread = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
            var track: AudioTrack = initialTrack!!
            var activeRate = initialRate
            val samples = ShortArray(1024)
            try {
                if (audioRunning) track.play()
                while (audioRunning) {
                    val requestedRate = nativeAudioRate()
                    if (requestedRate != activeRate) {
                        track.pause()
                        track.flush()
                        track.release()
                        if (!audioRunning) break
                        val replacement = createAudioTrack(requestedRate) ?: break
                        if (!audioRunning) {
                            replacement.release()
                            break
                        }
                        track = replacement
                        activeRate = requestedRate
                        audioTrack = replacement
                        track.play()
                    }
                    val count = nativeReadAudio(samples)
                    var offset = 0
                    while (offset < count && audioRunning) {
                        val written = track.write(samples, offset, count - offset, AudioTrack.WRITE_BLOCKING)
                        if (written <= 0) {
                            audioRunning = false
                            break
                        }
                        offset += written
                    }
                }
            } catch (t: Throwable) {
                if (audioRunning) runOnUiThread { status.text = "Audio Android detenido: ${t.message}" }
            } finally {
                try { track.stop() } catch (_: Throwable) { }
                try { track.release() } catch (_: Throwable) { }
                if (audioTrack === track) audioTrack = null
            }
        }, "OrcaCore-Audio").apply {
            isDaemon = true
            start()
        }
    }

    private fun startRumbleMonitor() {
        stopRumbleMonitor()
        rumbleRunning = true
        rumbleThread = Thread({
            while (rumbleRunning) {
                val next = nativeRumbleState()
                if (next != rumbleState) {
                    rumbleState = next
                    if (next == 0) {
                        vibrator?.cancel()
                    } else if (vibrator?.hasVibrator() == true) {
                        val amplitude = if (next >= 3) 220 else 110
                        vibrator.vibrate(VibrationEffect.createWaveform(
                            longArrayOf(0, 90), intArrayOf(amplitude, amplitude), 0
                        ))
                    }
                }
                try { Thread.sleep(20) } catch (_: InterruptedException) { break }
            }
            vibrator?.cancel()
            rumbleState = 0
        }, "OrcaCore-Rumble").apply {
            isDaemon = true
            start()
        }
    }

    private fun stopRumbleMonitor() {
        rumbleRunning = false
        rumbleThread?.interrupt()
        try { rumbleThread?.join(250) } catch (_: InterruptedException) { Thread.currentThread().interrupt() }
        rumbleThread = null
        rumbleState = 0
        vibrator?.cancel()
    }

    private fun stopAudio() {
        audioRunning = false
        try { audioTrack?.stop() } catch (_: Throwable) { }
        try { audioThread?.join(1000) } catch (_: InterruptedException) { Thread.currentThread().interrupt() }
        if (audioThread?.isAlive == true) {
            try { audioTrack?.release() } catch (_: Throwable) { }
            try { audioThread?.join(250) } catch (_: InterruptedException) { Thread.currentThread().interrupt() }
        }
        audioThread = null
        audioTrack = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        status = TextView(this).apply {
            text = "Iniciando Orca Core…"
            textSize = 13f
            setPadding(14, 10, 14, 10)
            gravity = Gravity.CENTER_VERTICAL
            setTextIsSelectable(true)
        }
        glView = GLSurfaceView(this).apply {
            setEGLConfigChooser { egl, display ->
                val attrs = intArrayOf(EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8,
                    EGL10.EGL_BLUE_SIZE, 8, EGL10.EGL_ALPHA_SIZE, 8, EGL10.EGL_DEPTH_SIZE, 16,
                    0x3040, EGL_OPENGL_ES3_BIT_KHR, EGL10.EGL_NONE)
                val configs = arrayOfNulls<EGLConfig>(1)
                val count = IntArray(1)
                check(egl.eglChooseConfig(display, attrs, configs, 1, count) && count[0] > 0) {
                    "No hay EGLConfig para OpenGL ES 3.x"
                }
                configs[0]!!
            }
            setEGLContextFactory(object : GLSurfaceView.EGLContextFactory {
                override fun createContext(egl: EGL10, display: EGLDisplay, config: EGLConfig): EGLContext {
                    val attrs32 = intArrayOf(EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL10.EGL_NONE)
                    val context32 = egl.eglCreateContext(display, config, EGL10.EGL_NO_CONTEXT, attrs32)
                    if (context32 != EGL10.EGL_NO_CONTEXT) return context32
                    val err = egl.eglGetError()
                    val attrs30 = intArrayOf(EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 0, EGL10.EGL_NONE)
                    val context30 = egl.eglCreateContext(display, config, EGL10.EGL_NO_CONTEXT, attrs30)
                    check(context30 != EGL10.EGL_NO_CONTEXT) {
                        "No se pudo crear GLES 3.2 ni 3.0 (0x${Integer.toHexString(err)})"
                    }
                    return context30
                }
                override fun destroyContext(egl: EGL10, display: EGLDisplay, context: EGLContext) {
                    egl.eglDestroyContext(display, context)
                }
            })
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                    val result = try { nativeInit() } catch (t: Throwable) { "Error GLES: ${t.message}" }
                    runOnUiThread {
                        status.text = result
                        if (result.startsWith("Disco GameCube cargado.")) startAudio()
                    }
                }
                override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
                    android.opengl.GLES20.glViewport(0, 0, width, height)
                }
                override fun onDrawFrame(gl: GL10?) {
                    android.opengl.GLES20.glClearColor(0.02f, 0.025f, 0.035f, 1f)
                    android.opengl.GLES20.glClear(android.opengl.GLES20.GL_COLOR_BUFFER_BIT or android.opengl.GLES20.GL_DEPTH_BUFFER_BIT)
                    nativeDrawFrame()
                }
            })
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }

        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(status, LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
        val open = Button(this).apply {
            text = "Abrir disco GameCube / Wii (.iso / .gcm)"
            setOnClickListener {
                AlertDialog.Builder(this@MainActivity)
                    .setTitle("Tipo de consola")
                    .setItems(arrayOf("GameCube", "Wii")) { _, which ->
                        pendingConsole = which
                        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                            addCategory(Intent.CATEGORY_OPENABLE)
                            type = "*/*"
                        }
                        startActivityForResult(intent, OPEN_DISC_REQUEST)
                    }
                    .show()
            }
        }
        root.addView(open, LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
        root.addView(glView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        root.addView(buildControls(), LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
        setContentView(root)
    }

    private fun buildControls(): View {
        val panel = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER }
        val directions = LinearLayout(this).apply { gravity = Gravity.CENTER }
        addPadButton(directions, "◀", PAD_LEFT)
        addPadButton(directions, "▲", PAD_UP)
        addPadButton(directions, "▼", PAD_DOWN)
        addPadButton(directions, "▶", PAD_RIGHT)
        panel.addView(directions)
        val actions = LinearLayout(this).apply { gravity = Gravity.CENTER }
        addPadButton(actions, "A", PAD_A)
        addPadButton(actions, "B", PAD_B)
        addPadButton(actions, "X", PAD_X)
        addPadButton(actions, "Y", PAD_Y)
        addPadButton(actions, "START", PAD_START)
        panel.addView(actions)
        return panel
    }

    private fun addPadButton(row: LinearLayout, label: String, bit: Int) {
        val button = Button(this).apply {
            text = label
            isAllCaps = false
            setPadding(4, 2, 4, 2)
            minWidth = 48
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> padButtons = padButtons or bit
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> padButtons = padButtons and bit.inv()
                    else -> return@setOnTouchListener true
                }
                publishPadState()
                true
            }
        }
        row.addView(button, LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT)
    }

    private fun publishPadState() {
        val buttons = padButtons
        val sx = stickX; val sy = stickY; val csx = cStickX; val csy = cStickY
        val tl = triggerL; val tr = triggerR
        glView.queueEvent { nativeSetPadState(buttons, sx, sy, csx, csy, tl, tr) }
    }

    private fun keyBit(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> PAD_A
        KeyEvent.KEYCODE_BUTTON_B -> PAD_B
        KeyEvent.KEYCODE_BUTTON_X -> PAD_X
        KeyEvent.KEYCODE_BUTTON_Y -> PAD_Y
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_ENTER -> PAD_START
        KeyEvent.KEYCODE_DPAD_LEFT -> PAD_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> PAD_RIGHT
        KeyEvent.KEYCODE_DPAD_UP -> PAD_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> PAD_DOWN
        else -> 0
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val bit = keyBit(event.keyCode)
        if (bit != 0 && (event.isFromSource(android.view.InputDevice.SOURCE_GAMEPAD) ||
                    event.isFromSource(android.view.InputDevice.SOURCE_DPAD) ||
                    event.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK))) {
            if (event.action == KeyEvent.ACTION_DOWN) padButtons = padButtons or bit
            else if (event.action == KeyEvent.ACTION_UP) padButtons = padButtons and bit.inv()
            publishPadState()
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if ((event.source and android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK &&
            event.action == MotionEvent.ACTION_MOVE) {
            fun axis(value: Float): Int = (128f + value.coerceIn(-1f, 1f) * 127f).toInt().coerceIn(0, 255)
            stickX = axis(event.getAxisValue(MotionEvent.AXIS_X))
            stickY = axis(-event.getAxisValue(MotionEvent.AXIS_Y))
            cStickX = axis(event.getAxisValue(MotionEvent.AXIS_Z))
            cStickY = axis(-event.getAxisValue(MotionEvent.AXIS_RZ))
            triggerL = (event.getAxisValue(MotionEvent.AXIS_LTRIGGER).coerceIn(0f, 1f) * 255f).toInt()
            triggerR = (event.getAxisValue(MotionEvent.AXIS_RTRIGGER).coerceIn(0f, 1f) * 255f).toInt()
            publishPadState()
            return true
        }
        return super.onGenericMotionEvent(event)
    }

    @Deprecated("Deprecated in Android API; retained for broad device compatibility")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri: Uri = data?.data ?: return
        if (requestCode == OPEN_DISC_REQUEST) {
            if (pendingConsole == 1) {
                pendingDiscUri = uri
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                }
                status.text = "Selecciona tu archivo de claves Wii (claves propias)."
                startActivityForResult(intent, OPEN_KEYS_REQUEST)
            } else {
                loadDisc(uri, 0, "")
            }
        } else if (requestCode == OPEN_KEYS_REQUEST) {
            status.text = "Leyendo claves y preparando disco Wii…"
            Thread {
                try {
                    val keyText = contentResolver.openInputStream(uri).use { input ->
                        requireNotNull(input) { "No se pudo abrir el archivo de claves." }
                        input.bufferedReader(Charsets.UTF_8).use { it.readText() }
                    }
                    runOnUiThread {
                        val discUri = pendingDiscUri
                        pendingDiscUri = null
                        if (discUri == null) {
                            status.text = "No se conservó la selección del disco Wii."
                        } else {
                            loadDisc(discUri, 1, keyText)
                        }
                    }
                } catch (t: Throwable) {
                    runOnUiThread { status.text = "Error al leer claves: ${t.message}" }
                }
            }.start()
        }
    }


    private fun loadDisc(uri: Uri, consoleKind: Int, keyText: String) {
        stopAudio()
        status.text = "Copiando imagen al almacenamiento privado…"
        Thread {
            try {
                val cached = File(cacheDir, if (consoleKind == 1) "selected_wii_disc.iso" else "selected_gamecube_disc.iso")
                contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "No se pudo abrir el archivo seleccionado." }
                    FileOutputStream(cached).use { output -> input.copyTo(output, 1024 * 1024) }
                }
                glView.queueEvent {
                    val result = try { nativeLoadDisc(cached.absolutePath, consoleKind, keyText) }
                    catch (t: Throwable) { "Error al cargar disco: ${t.message}" }
                    runOnUiThread {
                        status.text = result
                        if (result.contains("iniciado")) { startAudio(); startRumbleMonitor() }
                    }
                }
            } catch (t: Throwable) {
                runOnUiThread { status.text = "Error al copiar el disco: ${t.message}" }
            }
        }.start()
    }

    override fun onResume() { super.onResume(); glView.onResume() }
    override fun onPause() {
        stopRumbleMonitor()
        stopAudio()
        glView.queueEvent { nativeShutdown() }
        glView.onPause()
        super.onPause()
    }
}
