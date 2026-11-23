package com.redmi.rescue;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.provider.MediaStore;
import android.os.Environment;
import java.io.InputStream;
import java.util.Arrays;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.DocumentsContract;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

public final class ReportVault {
  private ReportVault(){}
  public static final int SAVE_TXT=51, SAVE_CSV=52;
  private static String diagnosis="", folders="", apps="", media="", root="";
  private static final ArrayList<String[]> rows=new ArrayList<>();
  private static final long GiB=1024L*1024L*1024L;
  public static synchronized void setDiagnosis(String s) { diagnosis=s; }
  public static synchronized void setFolderAnalysis(String text,List<String[]> entries) {
    folders=text;
    rows.removeIf(r->r.length>0 && ("Pasta".equals(r[0])||"Arquivo grande".equals(r[0])));
    for(String[] e:entries)rows.add(e.clone());
  }
  public static synchronized void setAppAnalysis(String text,List<String[]> entries) {
    apps=text;
    rows.removeIf(r->r.length>0 && ("Aplicativo".equals(r[0])||"Resumo Android".equals(r[0])));
    for(String[] e:entries)rows.add(e.clone());
  }
  public static synchronized void setRootAnalysis(String text,List<String[]> entries) {
    root=text;
    rows.removeIf(r->r.length>0 && r[0].startsWith("Root / "));
    for(String[] e:entries)rows.add(e.clone());
  }
  public static synchronized void setMedia(String s) { media=s; }
  public static synchronized boolean isEmpty() { return diagnosis.isEmpty()&&folders.isEmpty()&&apps.isEmpty()&&media.isEmpty()&&root.isEmpty(); }
  private static String stamp(){return new SimpleDateFormat("yyyy-MM-dd_HH-mm",Locale.ROOT).format(new Date());}
  public static void promptSave(Activity a,boolean csv) {
    Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT);
    intent.addCategory(Intent.CATEGORY_OPENABLE);
    intent.setType(csv?"text/csv":"text/plain");
    intent.putExtra(Intent.EXTRA_TITLE,"Redmi_Rescue_"+stamp()+(csv?".csv":".txt"));
    a.startActivityForResult(intent,csv?SAVE_CSV:SAVE_TXT);
  }
  private static String csvCell(String v) {
    if(v==null)v="";
    // Prevent Excel from interpreting filenames or app names as formulas.
    if(!v.isEmpty() && "=+-@\t\r\n".indexOf(v.charAt(0))>=0)v="'"+v;
    return "\""+v.replace("\"","\"\"")+"\"";
  }
  private static String csvLine(String...vals){
    StringBuilder b=new StringBuilder();
    for(int i=0;i<vals.length;i++){
      if(i>0)b.append(';');
      b.append(csvCell(vals[i]));
    }
    return b.append("\r\n").toString();
  }
  private static synchronized String asCsv() {
    StringBuilder s=new StringBuilder("\uFEFF");
    s.append(csvLine("Tipo","Nome","Tamanho (bytes)","Tamanho (GiB)","Contagem","Observação"));
    for(String[] row:rows){
      String bytes=row.length>2?row[2]:"0";
      long size=0;try{size=Long.parseLong(bytes);}catch(NumberFormatException ignored){}
      s.append(csvLine(row.length>0?row[0]:"",row.length>1?row[1]:"",bytes,
        String.format(Locale.forLanguageTag("pt-BR"),"%.3f",size/(double)GiB),
        row.length>3?row[3]:"",row.length>4?row[4]:""));
    }
    if(rows.isEmpty())s.append(csvLine("Aviso","Execute o mapa de armazenamento ou a análise de aplicativos", "0","0","0","Nenhuma análise tabular ainda"));
    return s.toString();
  }
  private static synchronized String asTxt() {
    return "REDMI RESCUE — RELATÓRIO LOCAL\n"+
       "Gerado em: "+new SimpleDateFormat("yyyy-MM-dd HH:mm:ss",Locale.getDefault()).format(new Date())+
       "\n\n=== DIAGNÓSTICO ===\n"+diagnosis+
       "\n\n=== MAPA DO ARMAZENAMENTO ===\n"+(folders.isEmpty()?"Não executado":folders)+
       "\n\n=== APLICATIVOS ===\n"+(apps.isEmpty()?"Não executado. Conceda Acesso ao uso e inicie a varredura guiada.":apps)+
       "\n\n=== MÍDIAS ===\n"+(media.isEmpty()?"Não executado":media)+
       "\n\n=== AUDITORIA ROOT (OPCIONAL) ===\n"+(root.isEmpty()?"Não executada / não necessária para a análise comum":root)+
       "\n\nAVISO: Não é um diagnóstico de hardware nem garantia de ganho de desempenho."+
       "\nNenhum arquivo foi apagado para gerar este relatório.\n";
  }
  public static synchronized String shareFullText() { return asTxt(); }

  /**
   * Saves a new app-owned report in public Downloads/RedmiRescue using MediaStore.
   * We never modify user files. A newly created incomplete report is removed on failure.
   */
  public static final class SavedReport {
    public final Uri uri;
    public final String filename;
    public final long bytes;
    public final String error;
    SavedReport(Uri u,String n,long b,String e) {uri=u;filename=n;bytes=b;error=e;}
    public boolean ok(){return uri!=null && bytes>0;}
  }
  public static SavedReport saveToDownloads(Activity activity,boolean csv){
    String name="Redmi_Rescue_"+stamp()+"_"+(System.currentTimeMillis()%100000L)+(csv?".csv":".txt");
    byte[] content=(csv?asCsv():asTxt()).getBytes(StandardCharsets.UTF_8);
    if(content.length<64) return new SavedReport(null,name,0,"Relatório ainda não tem dados suficientes.");
    ContentResolver resolver=activity.getContentResolver();
    Uri created=null;
    try{
      ContentValues values=new ContentValues();
      values.put(MediaStore.MediaColumns.DISPLAY_NAME,name);
      values.put(MediaStore.MediaColumns.MIME_TYPE,csv?"text/csv":"text/plain");
      values.put(MediaStore.MediaColumns.RELATIVE_PATH,Environment.DIRECTORY_DOWNLOADS+"/RedmiRescue");
      values.put(MediaStore.MediaColumns.IS_PENDING,1);
      created=resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI,values);
      if(created==null) throw new IllegalStateException("O Android não criou a entrada no Downloads.");
      try(OutputStream stream=resolver.openOutputStream(created,"w")){
        if(stream==null)throw new IllegalStateException("O arquivo não abriu para gravação.");
        stream.write(content);
        stream.flush();
      }
      ContentValues visible=new ContentValues();
      visible.put(MediaStore.MediaColumns.IS_PENDING,0);
      if(resolver.update(created,visible,null,null)<1)
        throw new IllegalStateException("O Android não confirmou o arquivo salvo.");
      // Read back all bytes to rule out an empty or truncated file.
      byte[] readback=new byte[content.length+1];
      int length=0;
      try(InputStream input=resolver.openInputStream(created)){
        if(input==null)throw new IllegalStateException("Não foi possível verificar o arquivo salvo.");
        int n;
        while(length<readback.length && (n=input.read(readback,length,readback.length-length))!=-1)length+=n;
      }
      if(length!=content.length)throw new IllegalStateException("Arquivo incompleto: "+length+" de "+content.length+" bytes.");
      for(int i=0;i<content.length;i++)if(content[i]!=readback[i])
        throw new IllegalStateException("O arquivo não corresponde ao relatório gerado.");
      return new SavedReport(created,name,content.length,null);
    }catch(Exception ex){
      if(created!=null)try{resolver.delete(created,null,null);}catch(Exception ignored){}
      return new SavedReport(null,name,0,ex.getClass().getSimpleName()+": "+ex.getMessage());
    }
  }

  public static boolean write(Activity a,Uri uri,boolean csv) {
    if(uri==null)return false;
    try(OutputStream out=a.getContentResolver().openOutputStream(uri,"w")){
      if(out==null)return false;
      out.write((csv?asCsv():asTxt()).getBytes(StandardCharsets.UTF_8));
      out.flush();
      return true;
    }catch(Exception e){return false;}
  }
}
