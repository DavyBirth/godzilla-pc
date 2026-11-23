package com.redmi.rescue;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.ContentUris;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class MainActivity extends Activity {
    static final int MEDIA_REQUEST=41, FILE_REQUEST=42;
    static final long MB=1024L*1024L, MAX_HASH=256L*MB, MAX_TOTAL=768L*MB;
    final ArrayList<MediaEntry> records=new ArrayList<>();
    final ArrayList<ArrayList<MediaEntry>> duplicates=new ArrayList<>();
    LinearLayout root; TextView output; boolean scanning=false;
    int background=Color.rgb(10,18,25), text=Color.rgb(231,246,240), accent=Color.rgb(132,243,201);
    String report="";
    static class MediaEntry {
        Uri uri; String name,folder; long size;
        MediaEntry(Uri u,String n,String p,long s){uri=u;name=n;folder=p;size=s;}
    }
    @Override public void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(background);
        getWindow().setNavigationBarColor(background);
        ScrollView scroll=new ScrollView(this);
        scroll.setFillViewport(true);
        root=new LinearLayout(this);root.setOrientation(1);root.setPadding(24,28,24,32);root.setBackgroundColor(background);
        scroll.addView(root);setContentView(scroll);
        label("REDMI RESCUE · BETA",22,accent);
        label("Sem root · sem internet · sem exclusão automática",13,text);
        button("Atualizar diagnóstico",()->diagnostics());
        button("Autorizar leitura das mídias",()->permissions());
        button("Mapear pastas acessíveis (somente leitura)",()->StorageAudit.start(this, output, s->{report += "\n\n"+s;}));
        button("Descobrir aplicativos que ocupam mais espaço",()->AppStorageAudit.start(this, output));
        button("Analisar arquivos grandes e duplicatas",()->startScan());
        button("Revisar um arquivo (não exclui)",()->pick());
        button("Salvar relatório CSV para Excel",()->ReportVault.promptSave(this,true));
        button("Salvar relatório TXT completo",()->ReportVault.promptSave(this,false));
        button("Compartilhar resumo sem anexos",()->share());
        output=label("Diagnóstico inicial",15,text);
        diagnostics();
    }
    TextView label(String s,int size,int color) {
        TextView v=new TextView(this);v.setText(s);v.setTextColor(color);v.setTextSize(size);v.setPadding(0,13,0,13);root.addView(v);return v;
    }
    void button(String title,Runnable action){
        Button b=new Button(this);b.setText(title);b.setAllCaps(false);
        b.setOnClickListener(v->action.run());root.addView(b);
    }
    void notice(String s){runOnUiThread(()->new AlertDialog.Builder(this).setTitle("Redmi Rescue").setMessage(s).setPositiveButton("OK",null).show());}
    static String gib(long b){return String.format(Locale.getDefault(),"%.2f GB",b/1073741824.0);}
    double temp(){
        Intent i=registerReceiver(null,new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int n=i==null?-1:i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,-1);
        return n<0?-1:n/10.0;
    }
    void diagnostics(){
        StatFs fs=new StatFs(Environment.getDataDirectory().getAbsolutePath());
        ActivityManager am=(ActivityManager)getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo mem=new ActivityManager.MemoryInfo();am.getMemoryInfo(mem);
        PowerManager power=(PowerManager)getSystemService(Context.POWER_SERVICE);
        double c=temp();int thermal=Build.VERSION.SDK_INT>=29?power.getCurrentThermalStatus():-1;
        String s="Dispositivo: "+Build.MANUFACTURER+" "+Build.MODEL+"\nAndroid: "+Build.VERSION.RELEASE+
          "\nArmazenamento livre: "+gib(fs.getAvailableBytes())+" de "+gib(fs.getTotalBytes())+
          "\nRAM disponível: "+gib(mem.availMem)+" de "+gib(mem.totalMem)+
          "\nTemperatura da bateria: "+(c<0?"indisponível":String.format(Locale.getDefault(),"%.1f °C",c))+
          "\nEstado térmico Android: "+thermal+
          "\n\nA RAM disponível é instantânea e não mede velocidade. A temperatura da bateria não é a temperatura do processador.";
        report=s;ReportVault.setDiagnosis(s);output.setText(s);
    }
    boolean allowed(){
        if(Build.VERSION.SDK_INT>=33)
            return checkSelfPermission(Manifest.permission.READ_MEDIA_IMAGES)==PackageManager.PERMISSION_GRANTED
                ||checkSelfPermission(Manifest.permission.READ_MEDIA_VIDEO)==PackageManager.PERMISSION_GRANTED
                ||checkSelfPermission(Manifest.permission.READ_MEDIA_AUDIO)==PackageManager.PERMISSION_GRANTED
                ||(Build.VERSION.SDK_INT>=34&&checkSelfPermission("android.permission.READ_MEDIA_VISUAL_USER_SELECTED")==PackageManager.PERMISSION_GRANTED);
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)==PackageManager.PERMISSION_GRANTED;
    }
    void permissions(){
        if(Build.VERSION.SDK_INT>=34)requestPermissions(new String[]{Manifest.permission.READ_MEDIA_IMAGES,Manifest.permission.READ_MEDIA_VIDEO,Manifest.permission.READ_MEDIA_AUDIO,"android.permission.READ_MEDIA_VISUAL_USER_SELECTED"},MEDIA_REQUEST);
        else if(Build.VERSION.SDK_INT>=33)requestPermissions(new String[]{Manifest.permission.READ_MEDIA_IMAGES,Manifest.permission.READ_MEDIA_VIDEO,Manifest.permission.READ_MEDIA_AUDIO},MEDIA_REQUEST);
        else requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE},MEDIA_REQUEST);
    }
    @Override public void onRequestPermissionsResult(int code,String[] p,int[] r){
        super.onRequestPermissionsResult(code,p,r);
        if(code==MEDIA_REQUEST)notice(allowed()?"Autorização recebida. Agora você pode analisar as mídias.":"Sem autorização. Os outros diagnósticos continuam disponíveis.");
    }
    static boolean protectedFile(String name,String path){
        String s=((name==null?"":name)+" "+(path==null?"":path)).toLowerCase(Locale.ROOT);
        return s.contains("whatsapp")||s.contains("telegram")||s.contains("signal")||s.contains("/android/")||s.contains("backup")||s.contains(".crypt")||s.contains(".db");
    }
    void read(Uri collection,List<MediaEntry> found){
        String[] fields={MediaStore.MediaColumns._ID,MediaStore.MediaColumns.DISPLAY_NAME,MediaStore.MediaColumns.RELATIVE_PATH,MediaStore.MediaColumns.SIZE};
        try(Cursor cur=getContentResolver().query(collection,fields,null,null,null)){
            if(cur==null)return;
            while(cur.moveToNext()&&found.size()<12000){
                String name=cur.getString(1), folder=cur.getString(2);
                long size=cur.getLong(3);
                if(size<=0||protectedFile(name,folder))continue;
                found.add(new MediaEntry(ContentUris.withAppendedId(collection,cur.getLong(0)),name,folder,size));
            }
        }catch(Exception ignored){}
    }
    String hash(Uri uri) throws Exception {
        MessageDigest md=MessageDigest.getInstance("SHA-256");
        try(InputStream in=getContentResolver().openInputStream(uri)){
            if(in==null)throw new IllegalStateException("Sem acesso");
            byte[] buf=new byte[65536];int n;
            while((n=in.read(buf))!=-1)md.update(buf,0,n);
        }
        StringBuilder s=new StringBuilder();
        for(byte b:md.digest())s.append(String.format(Locale.ROOT,"%02x",b&255));
        return s.toString();
    }
    void startScan(){
        if(scanning){notice("Análise já em andamento.");return;}
        if(!allowed()){permissions();return;}
        scanning=true;output.setText("Analisando mídias acessíveis (sem apagar nenhum arquivo)...");
        new Thread(()->{
            ArrayList<MediaEntry> items=new ArrayList<>();
            read(MediaStore.Images.Media.EXTERNAL_CONTENT_URI,items);
            read(MediaStore.Video.Media.EXTERNAL_CONTENT_URI,items);
            read(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI,items);
            items.sort((a,b)->Long.compare(b.size,a.size));
            Map<Long,List<MediaEntry>> bySize=new HashMap<>();
            long total=0;
            for(MediaEntry e:items){total+=e.size;if(e.size<=MAX_HASH)bySize.computeIfAbsent(e.size,k->new ArrayList<>()).add(e);}
            Map<String,ArrayList<MediaEntry>> groups=new HashMap<>();
            long bytesHashed=0;int compared=0,skipped=0;
            ArrayList<Long> sizes=new ArrayList<>(bySize.keySet());Collections.sort(sizes);
            for(long size:sizes){
                List<MediaEntry> candidates=bySize.get(size);
                if(candidates.size()<2)continue;
                if(temp()>=42){skipped+=candidates.size();break;}
                if(size*candidates.size()>MAX_TOTAL-bytesHashed){skipped+=candidates.size();continue;}
                for(MediaEntry e:candidates){
                    if(temp()>=42){skipped++;continue;}
                    try{
                        String key=e.size+":"+hash(e.uri);
                        groups.computeIfAbsent(key,k->new ArrayList<>()).add(e);
                        bytesHashed+=e.size;compared++;
                    }catch(Exception ex){skipped++;}
                }
            }
            ArrayList<ArrayList<MediaEntry>> dup=new ArrayList<>();long redundant=0;
            for(ArrayList<MediaEntry> list:groups.values())if(list.size()>1){
                dup.add(list);redundant+=(list.size()-1)*list.get(0).size;
            }
            dup.sort((a,b)->Long.compare(b.get(0).size,a.get(0).size));
            StringBuilder sb=new StringBuilder();
            sb.append("Arquivos de mídia acessíveis: ").append(items.size()).append("\n");
            sb.append("Total indexado: ").append(gib(total)).append("\n");
            sb.append("Comparados pelo conteúdo SHA-256: ").append(compared).append("\n");
            sb.append("Ignorados/indisponíveis: ").append(skipped).append("\n");
            sb.append("Grupos de duplicados verificados: ").append(dup.size()).append("\n");
            sb.append("Espaço potencial em cópias idênticas: ").append(gib(redundant)).append("\n");
            sb.append("Amostra limitada a 12 mil mídias e 768 MB em hashes. Sem acesso aos dados privados dos apps.\n\n");
            sb.append("MAIORES ARQUIVOS (somente leitura)\n");
            for(int i=0;i<Math.min(15,items.size());i++){
                MediaEntry e=items.get(i);sb.append(i+1).append(". ").append(e.name).append(" (").append(gib(e.size)).append(")\n");
            }
            sb.append("\nDUPLICATAS CONFIRMADAS (nomes ocultados no relatório compartilhado)\n");
            for(int i=0;i<Math.min(10,dup.size());i++)sb.append(i+1).append(". ").append(dup.get(i).size()).append(" cópias de ").append(gib(dup.get(i).get(0).size)).append("\n");
            String view=sb.toString();
            synchronized(records){records.clear();records.addAll(items);duplicates.clear();duplicates.addAll(dup);}
            final String summary="Mídias indexadas: "+items.size()+"\nGrupos duplicados: "+dup.size()+"\nPotencial: "+gib(redundant)+"\nComparados: "+compared+"\nIgnorados: "+skipped;
            runOnUiThread(()->{
                output.setText(view+"\nNenhum arquivo foi excluído.");
                ReportVault.setMedia(view);
                report+="\n\n"+summary;
                if(!dup.isEmpty())button("Revisar duplicatas (sem apagar)",()->reviewDuplicates());
                scanning=false;
            });
        },"media-analysis").start();
    }
    void reviewDuplicates(){
        ArrayList<ArrayList<MediaEntry>> copy;
        synchronized(records){copy=new ArrayList<>(duplicates);}
        if(copy.isEmpty()){notice("Nenhum grupo confirmado.");return;}
        StringBuilder s=new StringBuilder();
        for(int i=0;i<Math.min(10,copy.size());i++){
            ArrayList<MediaEntry> g=copy.get(i);s.append("\nGrupo ").append(i+1).append(" (").append(g.size()).append(" cópias):\n");
            for(MediaEntry e:g)s.append("• ").append(e.name).append(" — ").append(e.folder).append("\n");
        }
        new AlertDialog.Builder(this).setTitle("Duplicatas verificadas — apenas leitura")
          .setMessage(s+"\nNenhum arquivo será movido ou excluído nesta versão beta.")
          .setPositiveButton("OK",null).show();
    }
    void pick(){
        Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(i,FILE_REQUEST);
    }
    @Override protected void onActivityResult(int req,int code,Intent data){
        super.onActivityResult(req,code,data);
        if(req==ReportVault.SAVE_TXT||req==ReportVault.SAVE_CSV){
            if(code==RESULT_OK && data!=null && data.getData()!=null) {
                boolean saved=ReportVault.write(this,data.getData(),req==ReportVault.SAVE_CSV);
                notice(saved?"Relatório salvo! Abra o gerenciador Arquivos no local escolhido.":"Não foi possível salvar. Verifique o armazenamento e tente outra pasta.");
            }
            return;
        }
        if(req!=FILE_REQUEST||code!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();
        String name="arquivo",provider=uri.getAuthority();
        long size=0;
        try(Cursor c=getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE},null,null,null)){
            if(c!=null&&c.moveToFirst()){name=c.getString(0);if(!c.isNull(1))size=c.getLong(1);}
        }catch(Exception ignored){}
        boolean installer=name.toLowerCase(Locale.ROOT).matches(".*\\.(apk|apks|xapk|part|tmp|crdownload)$");
        String msg=name+"\nTamanho: "+gib(size)+"\nProvedor: "+provider+"\n\n"+
          (installer?"Instalador ou temporário detectado. Confira no gerenciador de arquivos do Android se realmente pode remover.":"Arquivo mantido. Não é um instalador/temporário reconhecido.");
        new AlertDialog.Builder(this).setTitle("Revisão de arquivo — sem exclusão")
          .setMessage(msg).setPositiveButton("OK",null).show();
    }
    void share(){
        Intent i=new Intent(Intent.ACTION_SEND);i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TEXT,"REDMI RESCUE BETA\n"+report+"\nNenhum dado foi enviado automaticamente.");
        startActivity(Intent.createChooser(i,"Compartilhar diagnóstico"));
    }
}
