"""Validate mixed depth counts against independent stencil writes and view routing."""
import argparse, copy, itertools, json
from pathlib import Path
from validate_depth_return_count import verify
from validate_pixel_return_count import read, require, sha

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();rows=[];rejected=0
    modes=('ordinary','depth-fail','shader-depth','discard','early-discard','coverage','integer-alpha-small','integer-alpha-one','integer-minimum','integer-signed','sparse','layer','empty','null','empty-depth','early-empty-discard','unbound')
    for vendor,samples,mode in itertools.product((0,4318,32902),(1,4),modes):
        path=args.root/f'depthColorCount-{vendor}-{samples}-{mode}.json';data=read(path)
        require([data[k] for k in ('vendor','samples','mode')]==[vendor,samples,mode], 'Case identity')
        require(len(data['rows'])==samples, 'Complete samples')
        for sample,row in enumerate(data['rows']):
            require(row['sample']==sample and row['observed_depth']==row['original_depth'], 'Original depth equality')
            offset=row['stencil_offset'];delta=row['original_depth'][offset]-row['initial_depth'][offset]
            expected=2 if mode in ('early-discard','early-empty-discard') else delta
            require(row['stencil_delta']==delta and (mode not in ('early-discard','early-empty-discard') or delta==3), 'Independent stencil delta')
            require(row['expected']==expected and 0<=expected<=3, 'Independent stencil count')
            if not mode.startswith('integer'):
                oracle=0 if mode in ('depth-fail','shader-depth','empty-depth') or (mode=='coverage' and sample%2) else 2 if mode in ('discard','early-discard','early-empty-discard') else 3
                require(expected==oracle, 'Owned draw model')
            report=row['observation'];verify(report,vendor,expected)
            depth=report['depth_transport'];color=report['count_transport']
            require(depth['source_first_layer']==(1 if mode=='layer' else 0)
                    and color['source_first_layer']==(2 if mode=='layer' else 0)
                    and depth['source_mip']==color['source_mip']==(1 if mode=='layer' and samples==1 else 0), 'Subresource domains')
            require(color['source_slot']==(7 if mode=='sparse' else 0) and color['sample']==sample, 'Source binding')
            added=mode in ('empty','null','empty-depth','early-empty-discard','unbound')
            require((report['additional_target_shader'] if added else report['completion_shader'])['source_sha256']==sha(Path(str(path)+'.original.dxbc')), 'Original PS identity')
            require(report['original_null_pixel_shader'] == (mode=='null'), 'Null shader identity')
            mutations=[lambda r:r['depth_transport'].update(width=99),lambda r:r['depth_transport'].update(sample=99),
                lambda r:r['depth_transport'].update(source_color_slot=99),lambda r:r['return_count'].update(count=99),
                lambda r:r.update(original_null_pixel_shader=not report['original_null_pixel_shader']),lambda r:r.update(original_fragment_eligibility=True)]
            for mutate in mutations:
                bad=copy.deepcopy(report);mutate(bad)
                try:verify(bad,vendor,expected)
                except (KeyError,ValueError):rejected+=1
                else:raise ValueError('Corrupt mixed count accepted')
            unknown=copy.deepcopy(report);unknown['return_count'].update(raw_bits=[0x4b800000]*4,count=None,valid=False,reason='float32_integer_limit')
            verify(unknown,vendor)
            try:verify(unknown,vendor,expected)
            except ValueError:rejected+=1
            else:raise ValueError('Unknown count accepted as measured reference')
            rows.append({'vendor':vendor,'samples':samples,'sample':sample,'mode':mode,'count':expected})
    output={'schema':1,'observations':len(rows),'corruptions_rejected':rejected,'cases':rows,'full_fragment_history_complete':False}
    args.out.write_text(json.dumps(output,indent=2)+'\n',encoding='utf-8');print(json.dumps({k:v for k,v in output.items() if k!='cases'}))
if __name__=='__main__':main()
