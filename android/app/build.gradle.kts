import com.android.build.api.artifact.SingleArtifact
import java.util.Properties
import java.util.zip.ZipFile

plugins {
    id("com.android.application")
}

// The game's code is generated on the desktop from the player's own disc (BUILDING.md): this
// build compiles the desktop checkout's gen/ folder. Its path is orta.desktopDir in
// android/local.properties, or -Porta.desktopDir=... on the command line (android/README.md).
val localProperties = Properties().apply {
    val file = rootProject.file("local.properties")
    if (file.exists()) file.inputStream().use { load(it) }
}
val desktopDir: String? =
    (findProperty("orta.desktopDir") as String?) ?: localProperties.getProperty("orta.desktopDir")

// Release signing, as in the owner's other ports: android/keystore.properties (never committed, see
// keystore.properties.example) names the keystore and its passwords. Without it, assembleRelease
// makes an unsigned APK.
val keystoreProperties = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.exists()) file.inputStream().use { load(it) }
}
val hasReleaseKeystore = keystoreProperties.getProperty("storeFile") != null

android {
    namespace = "io.github.wootbeer.ortarecomp"
    compileSdk = 35
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "io.github.wootbeer.ortarecomp"
        // Android 11: memfd_create in the C library (the guest's memory, xboxrecomp's platform layer).
        minSdk = 30
        targetSdk = 35
        // The same version name as the PC release it matches.
        versionCode = 1
        versionName = "0.1.12"

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                // PDO_RELEASE: the log in the app's storage (ortarecomp.log), as the PC releases.
                arguments += listOf("-DPDO_RELEASE=ON", "-DANDROID_STL=c++_shared")
                if (desktopDir != null) {
                    arguments += "-DPDO_GEN_DIR=$desktopDir/gen"
                }
                // libmain.so, the game with SDL linked in; nothing else the root project defines.
                targets += listOf("OrtaRecomp")
            }
        }
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            // Android Studio's Run builds debug. Unoptimised recompiled code is far too slow to
            // play, so the native side is optimised and keeps its debug info.
            externalNativeBuild {
                cmake {
                    arguments += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                }
            }
        }
        release {
            isMinifyEnabled = false
            if (hasReleaseKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    // SDL's own Java side (SDLActivity and the rest), straight from the SDL submodule, so it is
    // always the version the native side was built with.
    sourceSets {
        getByName("main") {
            java.directories.add("../../external/SDL/android-project/app/src/main/java")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    lint {
        abortOnError = false
    }
}

// The configure would build the launcher alone without the generated code; say so plainly instead.
tasks.matching { it.name.startsWith("configureCMake") }.configureEach {
    doFirst {
        if (desktopDir == null) {
            throw GradleException(
                "Set orta.desktopDir in android/local.properties to your desktop checkout " +
                "(where tools/recomp.py wrote gen/), e.g. orta.desktopDir=C:/dev/ortarecomp. See android/README.md.")
        }
        if (!file("$desktopDir/gen/recomp_dispatch.c").exists()) {
            throw GradleException(
                "No generated code in $desktopDir/gen: run tools/recomp.py on your disc there first (BUILDING.md).")
        }
    }
}

// The APK carries the compiled game code but nothing from the disc. Fails the build if a file in
// it is named like a disc image or an Xbox executable, or starts like an XBE ("XBEH").
abstract class CheckApkHasNoGameFiles : DefaultTask() {
    @get:InputFiles
    abstract val apkDir: DirectoryProperty

    @TaskAction
    fun check() {
        val extensions = listOf(".xbe", ".iso", ".xiso", ".sfd", ".aix", ".adx")
        val found = mutableListOf<String>()
        apkDir.get().asFileTree.matching { include("**/*.apk") }.forEach { apk ->
            ZipFile(apk).use { zip ->
                for (entry in zip.entries()) {
                    if (entry.isDirectory) continue
                    val named = extensions.any { entry.name.lowercase().endsWith(it) }
                    val header = ByteArray(4)
                    val read = zip.getInputStream(entry).use { it.readNBytes(header, 0, 4) }
                    val looksLikeXbe = read == 4 && header.contentEquals("XBEH".toByteArray())
                    if (named || looksLikeXbe) found += "${apk.name}: ${entry.name}"
                }
            }
        }
        if (found.isNotEmpty()) {
            throw GradleException(
                "The APK must not contain the game's files, but has:\n  " + found.joinToString("\n  ")
            )
        }
    }
}

// The license notices of everything built into the APK (the repo's licenses/ and COPYING), as
// tools/package.py gathers them for the PC releases: assets/licenses/ in the APK.
abstract class CollectLicenses : DefaultTask() {
    @get:InputFiles
    abstract val noticeFiles: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun collect() {
        val out = outputDir.get().dir("licenses").asFile
        out.deleteRecursively()
        out.mkdirs()
        noticeFiles.forEach { file ->
            val name = if (file.name == "COPYING") "OrtaRecomp-GPL-3.0.txt" else file.name
            file.copyTo(out.resolve(name), overwrite = true)
        }
    }
}

val collectLicenses = tasks.register<CollectLicenses>("collectLicenses") {
    val root = rootProject.layout.projectDirectory.dir("..")
    noticeFiles.from(root.file("COPYING"), root.dir("licenses").asFileTree)
    outputDir.set(layout.buildDirectory.dir("generated/licenses"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(collectLicenses, CollectLicenses::outputDir)

        val name = variant.name.replaceFirstChar { it.uppercase() }
        val check = tasks.register<CheckApkHasNoGameFiles>("check${name}ApkHasNoGameFiles") {
            apkDir.set(variant.artifacts.get(SingleArtifact.APK))
        }
        tasks.matching { it.name == "assemble$name" }.configureEach { dependsOn(check) }

        // Named like the PC downloads: OrtaRecomp-v<version>-android.apk (debug builds:
        // -android-debug.apk). VariantOutputImpl is AGP's own output type; the public API has no
        // file name.
        variant.outputs.forEach { output ->
            val suffix = if (variant.buildType == "release") "" else "-${variant.buildType}"
            val impl = output as? com.android.build.api.variant.impl.VariantOutputImpl
            if (impl != null) {
                impl.outputFileName.set(output.versionName.map { "OrtaRecomp-v$it-android$suffix.apk" })
            } else {
                logger.warn("APK file name left as AGP's default: its output type changed (${output.javaClass.name})")
            }
        }
    }
}
