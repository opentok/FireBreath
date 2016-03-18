'''
Fixes WiX installer files autogenerated by Heat in ATLCOM mode.

@author: Rob "N3X15" Nelson <nexisentertainment@gmail.com>
'''
import argparse, re, sys, os

from lxml import etree

wix_xmlns = {
    'w':"http://schemas.microsoft.com/wix/2006/wi",
    'xs':"http://www.w3.org/2001/XMLSchema",
    'fn':"http://www.w3.org/2005/xpath-functions"             
}
def FixWix(infile, outfile):
    tree = None
    with open(infile,'rb') as f:
        tree = etree.parse(f)
    for wComponent in tree.xpath('//w:Component', namespaces=wix_xmlns):
        #Directory="dir99DE416F55C8960850D5A4FCA3758AD4"
        if wComponent.get("Directory").startswith("dir"):
            wComponent.set("Directory","INSTALLDIR")
            print('>>> Fixed {}: Directory="INSTALLDIR"'.format(tree.getpath(wComponent)))
            
    for wFile in tree.xpath('//w:File', namespaces=wix_xmlns):
        #Source="SourceDir\Release\npHWLink.dll"
        if wFile.get("Source").startswith("SourceDir"):
            source = wFile.get("Source")
            # Source="$(var.BINSRC)\npHWLink.dll"
            wFile.set("Source","$(var.BINSRC)\\" + source.split('\\')[-1])
            print('>>> Fixed {}: Source="{}"'.format(tree.getpath(wFile),wFile.get("Source")))
            
    with open(outfile,'wb') as f:
        f.write(etree.tostring(tree,pretty_print=True))
if __name__ == '__main__':
    argp = argparse.ArgumentParser()
    argp.add_argument('input',type=str,help="Input WXS file.")
    argp.add_argument('output',type=str,help="Output WXS file.")
    args = argp.parse_args()
    FixWix(args.input, args.output)